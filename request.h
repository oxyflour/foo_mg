static void reg_string(struct lua_State *L, const char *name, const char *val) {
	lua_pushstring(L, name);
	lua_pushstring(L, val);
	lua_rawset(L, -3);
}

static void reg_int(struct lua_State *L, const char *name, lua_Integer val) {
	lua_pushstring(L, name);
	lua_pushinteger(L, val);
	lua_rawset(L, -3);
}

static int lsp_mg_print(lua_State *L) {
	int i, num_args;
	const char *str;
	size_t size;
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	num_args = lua_gettop(L);
	for (i = 1; i <= num_args; i++) {
		if (lua_isstring(L, i)) {
			str = lua_tolstring(L, i, &size);
			mg_write(conn, str, size);
		}
	}
	return 0;
}

static int lsp_mg_read(lua_State *L) {
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	char buf[1024];
	int len = mg_read(conn, buf, sizeof(buf));
	lua_settop(L, 0);
	lua_pushlstring(L, buf, len);
	return 1;
}

static int lsp_mg_get_var(lua_State *L) {
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char buf[1024];
	const char *varname = luaL_checkstring(L, 1);
	// use query string as content by default
	const char *content = ri->query_string;
	t_size size = ri->query_string != NULL ? strlen(ri->query_string) : 0;
	// if a second parameter is given, use that instead
	if (lua_gettop(L) >= 2) {
		content = luaL_checklstring(L, 2, &size);
	}
	if (content != NULL) {
		int len = mg_get_var(content, size, varname, buf, sizeof(buf));
		if (len > 0) {
			lua_pushlstring(L, buf, len);
			return 1;
		}
	}
	return 0;
}

static void init_lua_handle(struct mg_connection *conn, void *lua_context) {
	lua_State *L = (lua_State *)lua_context;
	const struct mg_request_info *ri = mg_get_request_info(conn);

	luaL_openlibs(L);
	luaopen_lsqlite3(L);
	luaopen_cjson(L);

	// Register "print" function which calls mg_write()
	lua_pushlightuserdata(L, conn);
	lua_pushcclosure(L, lsp_mg_print, 1);
	lua_setglobal(L, "print");

	// Register mg_read()
	lua_pushlightuserdata(L, conn);
	lua_pushcclosure(L, lsp_mg_read, 1);
	lua_setglobal(L, "read");

	// Register mg_get_var()
	lua_pushlightuserdata(L, conn);
	lua_pushcclosure(L, lsp_mg_get_var, 1);
	lua_setglobal(L, "get_var");

	// Export request_info
	lua_newtable(L);
	reg_string(L, "request_method", ri->request_method);
	reg_string(L, "uri", ri->uri);
	reg_string(L, "http_version", ri->http_version);
	reg_string(L, "query_string", ri->query_string);
	reg_int(L, "remote_ip", ri->remote_ip);
	reg_int(L, "remote_port", ri->remote_port);
	reg_int(L, "num_headers", ri->num_headers);
	lua_pushstring(L, "http_headers");
	lua_newtable(L);
	for (int i = 0; i < ri->num_headers; i++) {
		reg_string(L, ri->http_headers[i].name, ri->http_headers[i].value);
	}
	lua_rawset(L, -3);
	lua_setglobal(L, "request_info");

	// Push environment constants
	lua_newtable(L);
	reg_string(L, "doc_root",
		mg_get_option(g_init.get_static_instance().ctx, "document_root"));
	reg_string(L, "db_file_name", DB_FILE_NAME);
	reg_string(L, "db_track_table", DB_TRACK_TABLE);
	reg_string(L, "db_path_table", DB_PATH_TABLE);
	lua_setglobal(L, "fb_env");

	// Push control functions
	luaL_openlib(L, "fb_ctrl", fb_ctrl, 0);
	// Push control functions
	luaL_openlib(L, "fb_util", fb_util, 0);

	// Push stream functions
	lua_pushlightuserdata(L, conn);
	luaL_openlib(L, "fb_stream", fb_stream, 1);
}

static int begin_request_handler(struct mg_connection *conn) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	const char *doc_root = mg_get_option(g_init.get_static_instance().ctx, "document_root");
	pfc::string path = pfc::string8(doc_root) << ri->uri;
	lua_State *L = NULL;

	// load and exec lua script
	if (path.endsWith(".lua") &&
		uFileExists(path.get_ptr()) &&
		(L = luaL_newstate()) != NULL) {
		init_lua_handle(conn, L);
		if (luaL_loadfile(L, path.get_ptr()) != LUA_OK ||
			lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
			FOO_LOG << "lua: " << lua_tostring(L, -1);
		}
		lua_close(L);
		return 1;
	}
	return 0;
}