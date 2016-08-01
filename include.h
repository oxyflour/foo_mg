#include <time.h>
#include <Shlwapi.h>

// http://stackoverflow.com/questions/22265610/why-ssize-t-in-visual-studio-2010-is-defined-as-unsigned
#include <BaseTsd.h>

#include "../SDK/foobar2000.h"
#include "../helpers/input_helpers.h"

namespace arv {
	#include "../../libarchive/libarchive/archive.h"
	#include "../../libarchive/libarchive/archive_entry.h"
}

#include "../../lame-3.99.5/include/lame.h"

#include "../../mongoose-4.1/mongoose.h"
#include "../../mongoose-4.1/build/sqlite3.h"

extern "C" {
	#include "../../mongoose-4.1/build/lua_5.2.1.h"
	LUALIB_API void (luaL_openlibs) (lua_State *L);
	LUALIB_API int luaopen_lsqlite3(lua_State *L);
	LUALIB_API int luaopen_cjson(lua_State *l);
};

#define FOO_LOG console::formatter() << "foo_mg: "

#define DB_FILE_NAME "\\user-components\\foo_mg\\mgdatabase.db3"
#define DB_TRACK_TABLE "fb_track"
#define DB_PATH_TABLE "fb_path"

#define CONFIG_FILE "mongoose.conf"
#define CONFIG_REFERENCE "https://github.com/cesanta/mongoose/blob/52e3be5c58bf5671d0cc010e520395bc308326b4/UserManual.md"
#define MAX_OPTIONS 40
#define MAX_CONF_FILE_LINE_SIZE (8 * 1024)

#define DEFAULT_DOC_ROOT "\\user-components\\foo_mg\\www"
#define SCRIPT_SUFFIX ".lua"
#define HEADER_SERVER_NAME "foo_mg streamer"

#define strdup _strdup

enum ITEMS_ACTION {
	ACTION_ADD,
	ACTION_REMOVE,
	ACTION_MODIFY
};

__int64 FileTimeToSeconds(FILETIME ft) {
	ULARGE_INTEGER ift = {ft.dwLowDateTime, ft.dwHighDateTime};
	return ift.QuadPart/10000000 - 11644473600LL;
}

int log_message_handle(const struct mg_connection *, const char *message) {
	FOO_LOG << "mongoose: " << message;
	return 0;
}

pfc::string8 get_gmt_date(const time_t current) {
	tm timer;
	char dateStr[128];
	gmtime_s(&timer, &current);
	strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y %H:%M:%S GMT", &timer);
	return pfc::string8(dateStr);
}

pfc::string8 guess_img_type(const void *pdata) {
	//...
	// should be complete
	return pfc::string8("jpeg");
}

struct WAVE_FORMAT_HEADER {
	// RIFF WAVE CHUNK
	char szRiffID[4];  // 'R','I','F','F'
	DWORD dwRiffSize; // file size - 8
	char szRiffFormat[4]; // 'W','A','V','E'

	// FORMAT CHUNK
	char szFmtID[4]; // 'f', 'm', 't', ' '
	DWORD dwFormatSize; // 16 or 18 (for wExtra)
	WORD wFormatTag; // 0x001
	WORD wChannels; // 1 or 2
	DWORD dwSamplesPerSec;
	DWORD dwAvgBytesPerSec;
	WORD wBlockAlign;
	WORD wBitsPerSample;
//	WORD wExtra;

	// DATA CHUNK
	char szDataID[4]; // 'd','a','t','a'
	DWORD dwDataSize; // size of data
};

class database_handle {
	sqlite3 *db_handle;
	char *db_path;
public:
	database_handle(const char *fname) {
		db_handle = NULL;
		db_path = strdup(fname);
		int ret = sqlite3_open(db_path, &db_handle);
		if (ret != SQLITE_OK || db_handle == NULL) {
			const char *err = db_handle ? sqlite3_errmsg(db_handle) : "no msg";
			FOO_LOG << "create or open " << db_path << " error !(err" << ret << ": " << (err ? err : "") << ")";
			db_handle = NULL;
		}
	}
	~database_handle() {
		if (db_handle != NULL) {
			sqlite3_close(db_handle);
		}
		if (db_path != NULL) {
			free(db_path);
		}
	}
	int dump(database_handle *to) {
		sqlite3_backup *bk = sqlite3_backup_init(to->db_handle, "main", db_handle, "main");
		if (bk) {
			sqlite3_backup_step(bk, -1);
			sqlite3_backup_finish(bk);
		}
		return sqlite3_errcode(to->db_handle);
	}
	int exec(const char *sql, int (*callback)(void*,int,char**,char**), void *para, char **err) {
		if (db_handle != NULL) {
			return sqlite3_exec(db_handle, sql, callback, para, err);
		}
		else {
			if (err != NULL) {
				const char *msg = "invalid handle";
				t_size size = strlen(msg);
				*err = (char *)sqlite3_malloc(size + 1);
				memcpy(*err, msg, size + 1);
			}
			return SQLITE_MISUSE;
		}
	}
	const char *const getPath() {
		return db_path;
	}
};

class c_initquit : public initquit {
	lua_State *lua;
	CRITICAL_SECTION cs;

	database_handle *db;
	struct mg_context *ctx;

	bool started;

public:
	void on_init();
	void on_quit();

	lua_State *const acquireLua() {
		EnterCriticalSection(&cs);
		return lua;
	}
	void releaseLua() {
		LeaveCriticalSection(&cs);
	}

	database_handle *getDb() {
		return db;
	}
	bool isStarted() {
		return started;
	}

	const char *const getDbPath() {
		return db->getPath();
	}
	const char *const getDocRoot() {
		return mg_get_option(ctx, "document_root");
	}
	const int getListenPort() {
		return atoi(mg_get_option(ctx, "listening_ports"));
	}
};
static initquit_factory_t<c_initquit> g_init;

class c_library_callback : public library_callback {
public:
	void on_items_added(const pfc::list_base_const_t<metadb_handle_ptr> & p_data);
	void on_items_removed(const pfc::list_base_const_t<metadb_handle_ptr> & p_data);
	void on_items_modified(const pfc::list_base_const_t<metadb_handle_ptr> & p_data);
};
static library_callback_factory_t<c_library_callback> g_lib_callback;

//! SPECIAL WARNING: to allow multi-CPU optimizations to parse relative track paths, 
// this API works in threads other than the main app thread. 
// Main thread MUST be blocked while working in such scenarios, 
// it's NOT safe to call from worker threads while the Media Library content/configuration might be getting altered.
class titleformat_relative_path_hook : public titleformat_hook {
	metadb_handle_ptr &m_item;
	static_api_ptr_t<library_manager> &m_lib;
public:
	titleformat_relative_path_hook(metadb_handle_ptr &item, static_api_ptr_t<library_manager> &lib) : m_item(item), m_lib(lib) { }
	bool process_field(titleformat_text_out * p_out,const char * p_name,t_size p_name_length,bool & p_found_flag) {
		if (!strcmp(p_name, "relative_path")) {
			pfc::string8 path, rpath, pos;
			try {
				m_lib->get_relative_path(m_item, rpath);
				path = m_item->get_path();
				t_size sz = path.find_last('\\', path.get_length() - rpath.get_length() - 2) + 1;
				if (sz > path.get_length()) sz = 0;
				p_out->write(titleformat_inputtypes::unknown, path.get_ptr()+sz, path.get_length()-sz);
				return true;
			}
			catch (...) { }
			return true;
		}
		if (!strcmp(p_name, "path_index")) {
			pfc::string8 path, rpath, pos;
			try {
				m_lib->get_relative_path(m_item, rpath);
				path = m_item->get_path();
				if (strncmp(path.get_ptr(), "file://", 7)) {
					return false;
				}
				else {
					path = pfc::string8(path + 7);
				}
			}
			catch (...) {
				return false;
			}
			t_size p = path.find_last('\\', path.get_length() - rpath.get_length() - 2) + 1;
			pos << pfc::format_int(p > path.length() ? 0 : p, 3).toString();
			while ((p = path.find_first('\\', p+1)) != ~0) {
				pos << pfc::format_int(strlen_utf8(path, p), 3).toString();
			}
			p_out->write(titleformat_inputtypes::unknown, pos.get_ptr());
			return true;
		}
		return false;
	}
	bool process_function(titleformat_text_out * p_out,const char * p_name,t_size p_name_length,titleformat_hook_function_params * p_params,bool & p_found_flag) {
		return false;
	}
};

class titleformat_sql_char_filter : public titleformat_text_filter {
public :
	void write(const GUID & p_inputtype,pfc::string_receiver & p_out,
			const char * p_data,t_size p_data_length) {
		t_size i, j = 0;
		for (i = 0; i < p_data_length && p_data[i] != 0; i ++) {
			if (p_data[i] == '\'') {
				p_out.add_string(p_data + j, i - j);
				p_out.add_char('\'');
				j = i;
			}
		}
		p_out.add_string(p_data + j, p_data_length - j);
	}
};

// forward declaration
static void reg_int(struct lua_State *L, const char *name, lua_Integer val);
static void reg_string(struct lua_State *L, const char *name, const char *val);