void on_items_callback(database_handle *db, const pfc::list_base_const_t<metadb_handle_ptr> *list, ITEMS_ACTION act) {
	char *err, *tpl, *cmd;
	if (act == ACTION_ADD) {
		tpl = "INSERT OR IGNORE INTO `"DB_PATH_TABLE"` '('"
				"directory_path, "
				"relative_path, "
				"path_index, "
				"add_date"
			"')' VALUES '('"
				"''$directory_path(%path%)\\'', "
				"''$directory_path(%relative_path%)\\'', "
				"''%path_index%'', "
				"DATETIME'('''now'', ''localtime''')'"
			"')';"
			"INSERT OR REPLACE INTO `"DB_TRACK_TABLE"` '('"
				"title, "
				"artist, "
				"album_artist, "
				"album, "
				"date, "
				"genre, "
				"tracknumber, "
				"codec, "
				"filename_ext, "
				"pid, "
				"subsong, "
				"length, "
				"length_seconds, "
				"last_modified"
			"')' VALUES '('"
				"''%title%'', "
				"''%artist%'', "
				"''%album artist%'', "
				"''%album%'', "
				"''%date%'', "
				"''%genre%'', "
				"''%tracknumber%'', "
				"''%codec%'', "
				"''%filename_ext%'', "
				"'('SELECT id from `"DB_PATH_TABLE"` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')', "
				"%subsong%, "
				"''%length%'', "
				"%length_seconds%, "
				"''%last_modified%''"
			"')'; ";
		cmd = "add";
	}
	else if (act == ACTION_REMOVE) {
		tpl = "DELETE FROM `"DB_TRACK_TABLE"` "
			"WHERE pid='('SELECT id FROM `"DB_PATH_TABLE"` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')' "
				"AND filename_ext=''%filename_ext%'' "
				"AND subsong=%subsong%; "
			"DELETE FROM `"DB_PATH_TABLE"` WHERE directory_path=''$directory_path(%path%)\\'' "
				"AND NOT EXISTS '('SELECT  `"DB_TRACK_TABLE"`.id FROM `"DB_TRACK_TABLE"` LEFT JOIN `"DB_PATH_TABLE"` "
					"ON pid=`"DB_PATH_TABLE"`.id WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')';";
		cmd = "remove";
	}
	else if (act == ACTION_MODIFY) {
		tpl = "UPDATE `"DB_TRACK_TABLE"` SET "
			"`title`=''%title%'', "
			"`artist`=''%artist%'', "
			"`album_artist`=''%album artist%'', "
			"`album`=''%album%'', "
			"`date`=''%date%'', "
			"`genre`=''%genre%'', "
			"`tracknumber`=''%tracknumber%'', "
			"`codec`=''%codec%'', "
			"`length`=''%length%'', "
			"`length_seconds`=%length_seconds%, "
			"`last_modified`=''%last_modified%'' "
			"WHERE `pid`='('SELECT id FROM `"DB_PATH_TABLE"` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')' "
				"AND filename_ext=''%filename_ext%'' "
				"AND subsong=%subsong%;";
		cmd = "update";
	} 
	else {
		// Do nothing
		return;
	}

	// Prepare formatter
	static_api_ptr_t<titleformat_compiler> tfc;
	service_ptr_t<titleformat_object> sqlfmt;
	tfc->compile_force(sqlfmt, tpl);

	// Do update
	DWORD tick = GetTickCount();
	pfc::string8_fastalloc sql;
	titleformat_sql_char_filter flt = titleformat_sql_char_filter();
	static_api_ptr_t<library_manager> lib;
	t_size len = list->get_count(), size = 0;

	// use transaction to speed up
	db->exec("begin transaction;", NULL, NULL, &err);
	for (t_size i = 0; i < len; i ++) {
		metadb_handle_ptr item = list->get_item(i);
		titleformat_relative_path_hook hook =
			titleformat_relative_path_hook(item, lib);
		item->format_title(&hook, sql, sqlfmt, &flt);
		// execute sql
		int ret = db->exec(sql.get_ptr(), 0, NULL, &err);
		if (ret != SQLITE_OK) {
			FOO_LOG << cmd << " item failed !(err" << ret << ": " << (err ? err : "") << ")";
			sqlite3_free(err);
		}
	}
	db->exec("commit transaction;", NULL, NULL, &err);

	FOO_LOG << cmd << " " << list->get_count() << " items done (in " <<
		pfc::format_float((GetTickCount() - tick)/1000.0f, 0, 3) << "s)";
}

void init_database() {
	int ret;
	char *err;

	// foobar2000 trigger item add event when it startup
	ret = g_db.exec("CREATE TABLE	`"DB_PATH_TABLE"` ("
			"id INTEGER PRIMARY KEY, "
			"directory_path VARCHAR(512), "
			"relative_path VARCHAR(512) UNIQUE, "
			"path_index VARCHAR(512), "
			"add_date DATETIME"
		"); "
		"CREATE TABLE `"DB_TRACK_TABLE"` ("
			"id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"title VARCHAR(256), "
			"artist VARCHAR(256), "
			"album_artist VARCHAR(256), "
			"album VARCHAR(256), "
			"date VARCHAR(256), "
			"genre VARCHAR(256), "
			"tracknumber INTEGER, "
			"codec VARCHAR(256), "
			"filename_ext VARCHAR(256), "
			"pid INTEGER, "
			"subsong INTEGER, "
			"length VARCHAR(256), "
			"length_seconds INTEGER, "
			"last_modified DATETIME, "
			"UNIQUE (pid, filename_ext, subsong) ON CONFLICT REPLACE"
		");", 0, NULL, &err);
	if (ret != SQLITE_OK) {
		FOO_LOG << "create table failed !(err" << ret << ": " << (err ? err : "") << ")";
		sqlite3_free(err);
		return;
	}
	FOO_LOG << "initialize library ("DB_FILE_NAME") OK";

	static_api_ptr_t<library_manager> lib;
	pfc::list_t<metadb_handle_ptr> list;
	lib->get_all_items(list);
	// Add all items
	on_items_callback(&g_db, &list, ACTION_ADD);
}