#include "include.h"
#include "luacmd.h"
#include "database.h"
#include "request.h"

DECLARE_COMPONENT_VERSION("Foobar mongoose plugin", "0.0.1",
	"mongoose server with lua support");
VALIDATE_COMPONENT_FILENAME("foo_mg.dll");

// Load configure file if there is one. From mongoose/main.c
void load_conf_file(FILE *fp, char **options) {
	char line[MAX_CONF_FILE_LINE_SIZE], opt[sizeof(line)], val[sizeof(line)];
	size_t line_no = 0;
	int i, j;

	while (fgets(line, sizeof(line), fp) != NULL) {
		line_no++;

		// Ignore empty lines and comments
		for (i = 0; isspace(* (unsigned char *) &line[i]); ) i++;
		if (line[i] == '#' || line[i] == '\0') {
			continue;
		}

		// Invalid line?
		if (sscanf_s(line, "%s %[^\r\n#]", opt, sizeof(opt), val, sizeof(val)) != 2) {
			FOO_LOG << "line " << line_no << " is invalid in "CONFIG_FILE", ignoring it:\n" << line;
			continue;
		}

		// Find an new empty line in options array
		for (j = 0; j < MAX_OPTIONS - 3 && options[j] != NULL; ) j++;
		if (j < MAX_OPTIONS - 3) {
			options[j] = strdup(opt);
			options[j + 1] = strdup(val);
			options[j + 2] = NULL;
		}
		else {
			FOO_LOG << "Too many options specified, this line will be ignored:\n" << line;
		}
	}
}

void c_initquit::on_init() {
	FILE *fp;
	char *options[MAX_OPTIONS] = { NULL };
	struct mg_callbacks callbacks;

	// Try loading options from CONFIG_FILE
	fopen_s(&fp, CONFIG_FILE, "r");
	if (fp == NULL) {
		FOO_LOG << "no configure file found. Create "CONFIG_FILE" if you want to change mongoose settings";
	}
	else {
		FOO_LOG << "loading "CONFIG_FILE;
		load_conf_file(fp, options);
		fclose(fp);
	}

	// Override the default document directory
	int j;
	for (j = 0; j < MAX_OPTIONS - 3
		&& options[j] != NULL
		&& strcmp(options[j], "document_root"); ) j++;
	if (j < MAX_OPTIONS - 3 && options[j] == NULL) {
		options[j] = strdup("document_root");
		options[j+1] = strdup("www");
		options[j+2] = NULL;
	}

	// Prepare callbacks structure
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.begin_request = begin_request_handler;
//	callbacks.init_lua = init_lua_handle;
	callbacks.log_message = log_message_handle;

	// Start the web server
	ctx = mg_start(&callbacks, NULL, (const char **)options);
	if (ctx == NULL) {
		FOO_LOG << "server start failed";
	}
	else {
		FOO_LOG << "server started at port " << mg_get_option(ctx, "p") <<
			", document directory: " << mg_get_option(ctx, "r");
	}

	// Release memory allocated when loading config file
	for (int i = 0; options[i] != NULL; i ++) {
		free(options[i]);
	}

	if (g_db.exec("SELECT * from `"DB_TRACK_TABLE"` LIMIT 0,1", 0, NULL, NULL) != SQLITE_OK) {
		init_database();
	}
	started = true;
}

void c_initquit::on_quit() {
	// Stop the web server.
	if (ctx != NULL) {
		mg_stop(ctx);
	}

	started = false;
}

void c_library_callback::on_items_added(const pfc::list_base_const_t<metadb_handle_ptr> & p_data) {
	if (g_init.get_static_instance().started) {
		on_items_callback(&g_db, &p_data, ACTION_ADD);
	}
}

void c_library_callback::on_items_removed(const pfc::list_base_const_t<metadb_handle_ptr> & p_data) {
	// i don't want to lose my data (item add time and id) after foobar2000 shutdown,
	// so only remove them when startup
	if (g_init.get_static_instance().started) {
		on_items_callback(&g_db, &p_data, ACTION_REMOVE);
	}
}

void c_library_callback::on_items_modified(const pfc::list_base_const_t<metadb_handle_ptr> & p_data) {
	on_items_callback(&g_db, &p_data, ACTION_MODIFY);
}