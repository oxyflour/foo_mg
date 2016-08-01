static int lsp_play_pause(lua_State *L) {
	static_api_ptr_t<playback_control> pc;
	if (lua_gettop(L) > 0) {
		if (lua_isnil(L, 1)) {
			pc->toggle_pause();
		}
		else {
			pc->pause(lua_toboolean(L, 1) != 0);
		}
	}
	lua_pushboolean(L, pc->is_paused());
	return 1;
}

static int lsp_start_stop(lua_State *L) {
	static_api_ptr_t<playback_control> pc;
	if (lua_gettop(L) > 0) {
		if (lua_isnil(L, 1)) {
			pc->play_or_pause();
		}
		else {
			lua_toboolean(L, 1) ? pc->play_start() : pc->stop();
		}
	}
	lua_pushboolean(L, pc->is_playing());
	return 0;
}

static const struct luaL_Reg fb_ctrl[] = {
	{"play_pause", lsp_play_pause},
	{"start_stop", lsp_start_stop},
	{NULL, NULL}
};

static int get_albumart(mg_connection *conn, const char *fsave, const char *fpath) {
	mg_request_info *ri = mg_get_request_info(conn);
	abort_callback_dummy cb;
	album_art_manager_instance_ptr ami = static_api_ptr_t<album_art_manager>()->instantiate();
	if (ami->open(fpath, cb)) {
		try {
			album_art_data_ptr data = ami->query(album_art_ids::cover_front, cb);
			t_size size = data->get_size();
			const void *pdata = data->get_ptr();
			if (size > 0) {
				pfc::string8 head = pfc::string_formatter() <<
					"HTTP/1.0 200 OK\r\n"
					"Date: " << get_gmt_date(time(NULL)) << "\r\n"
					"Server: "HEADER_SERVER_NAME"\r\n"
					"Content-type: image/" << guess_img_type(pdata) << "\r\n"
					"Content-length: " << size << "\r\n"
					"\r\n";
				if (fsave != NULL) {
					DWORD writebytes;
					HANDLE hFile = uCreateFile(fsave, GENERIC_WRITE, 0, NULL,
						CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
					WriteFile(hFile, pdata, size, &writebytes, NULL);
					CloseHandle(hFile);
				}
				else if (conn != NULL) {
					mg_write(conn, head.get_ptr(), head.get_length());
					mg_write(conn, pdata, size);
				}
			}
			return size;
		}
		catch (std::exception &e) {
			FOO_LOG << "stream[" << ri->remote_port << "]: " << e.what() << ", file: \n" << fpath;
		}
		catch (...) {
			FOO_LOG << "stream[" << ri->remote_port << "]: error, file: \n" << fpath;
		}
	}
	return -1;
}

static int lsp_stream_albumart(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	lua_pushinteger(L, get_albumart(conn, NULL, fpath));
	return 1;
}

static int lsp_extract_albumart(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	const char *fsave = luaL_checkstring(L, 2);
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	lua_pushinteger(L, get_albumart(conn, fsave, fpath));
	return 1;
}

static WAVE_FORMAT_HEADER create_wave_header(WORD channels, DWORD srate, DWORD dataSize, WORD wavbit) {
	WAVE_FORMAT_HEADER wf;

	memcpy_s(wf.szRiffID, 4, "RIFF", 4);
	wf.dwRiffSize = dataSize + sizeof(WAVE_FORMAT_HEADER) - 8;
	memcpy_s(wf.szRiffFormat, 4, "WAVE", 4);

	memcpy_s(wf.szFmtID, 4, "fmt ", 4);
	wf.dwFormatSize = 16;
	wf.wFormatTag = 0x001;
	wf.wChannels = channels;
	wf.dwSamplesPerSec = srate;
	wf.dwAvgBytesPerSec = wf.dwSamplesPerSec * wavbit / 8 * wf.wChannels;
	wf.wBlockAlign = wavbit / 8 * wf.wChannels;
	wf.wBitsPerSample = wavbit;
//	wf.wExtra = 0;

	memcpy_s(wf.szDataID, 4, "data", 4);
	wf.dwDataSize = dataSize;
	return wf;
}

// This function returns the opened input_helper and try to decode the first chunk
// to guess the track data length in bytes & content length in bytes
static void get_track_info(const char *fpath, int subsong, double seek, int wavbit,
		input_helper &ih, audio_chunk &chunk, int &trackbytes, int &contentbytes) {
	file_info_impl fi;
	abort_callback_dummy cb;

	// open file and get information
	ih.open_path(NULL, fpath, cb, false, false);
	ih.open_decoding(subsong, 0, cb);
	ih.get_info(subsong, fi, cb);
	if (seek > 0 && ih.can_seek()) {
		ih.seek(seek, cb);
	}
	else {
		seek = 0;
	}

	// get first chunk
	ih.run(chunk, cb);

	// guess track total bytes;
	if (wavbit > 0) {
		trackbytes = (int)((fi.get_length() - seek) *
			chunk.get_sample_rate() * chunk.get_channel_count() * wavbit / 8);
		contentbytes = sizeof(WAVE_FORMAT_HEADER) + trackbytes;
	}
	else {
		trackbytes = contentbytes = 0;
	}

}

// This function returns the begin and end bytes from the "Range" header string
static bool get_request_range(const char *rangeStr, int maxbytes,
		int &begin, int &end) {
	if (rangeStr != NULL) {
		sscanf_s(rangeStr, "bytes=%d-%d", &begin, &end);
	}
	if (begin < 0) {
		begin = 0;
	}
	if (end < begin || end > maxbytes - 1) {
		end = maxbytes > 0 ? maxbytes - 1 : -1;
	}
	return true;
}

// Bytes from begin & end + 1 are sent, and current byte index are set by the second parameter.
// The return value is the length of bytes that have been sent, if a correct "current" value is given
static int send_data_range(mg_connection *conn, int &current, char *buf, t_size size,
		int begin, int end) {
	int tosend = size, offset = 0, sended = 0;
	if (current + (int)size > begin) {
		if (current < begin) {
			offset = begin - current;
			tosend = current + size - begin;
		}
		else {
			sended = current - begin;
		}
		if (end > begin && current + tosend > end + 1) {
			tosend = end + 1 - current;
		}
		if (mg_write(conn, buf + offset, tosend) != tosend) {
			throw std::exception("http send failed");
		}
		sended += tosend;
	}
	current += size;
	return sended;
}

static int get_track(mg_connection *conn, const char *fpath,
		t_uint32 subsong, double seek, const lame_t lame, t_uint32 wavbit) {
	abort_callback_dummy cb;
	input_helper ih;
	audio_chunk_impl_temporary chunk;
	static_api_ptr_t<audio_postprocessor> proc;
	mem_block_container_impl_t<> buf;

	mg_request_info *ri = mg_get_request_info(conn);
	const char *rangestring = mg_get_header(conn, "Range");
	bool sendrange = (rangestring != NULL && lame == NULL);
	int current = 0, sended = 0, begin = -1, end = -1;
	int trackbytes = 0, contentbytes = 0;
	try {
		get_track_info(fpath, subsong, seek, wavbit,
			ih, chunk, trackbytes, contentbytes);
		get_request_range(rangestring, contentbytes,
			begin, end);
	}
	catch (std::exception &e) {
		FOO_LOG << "stream: open error: " << e.what() << ", file: \n" << fpath;
		ih.close();
		return -1;
	}

	FOO_LOG << "stream[" << ri->remote_port << "]: decoding file: \n" << fpath << ", subsong " << subsong << ", "
		"(" << (lame != NULL ? "mp3" : "wav") <<
		(begin > 0 || end >= begin ? pfc::string8(", ") << "range " << begin << "~" << end : "") << 
		")";

	try {
		// prepare headers
		pfc::string8 header = pfc::string_formatter() <<
			(sendrange ? "HTTP/1.0 206 Partial Content\r\n" : "HTTP/1.0 200 OK\r\n") <<
			"Date: " << get_gmt_date(time(NULL)) << "\r\n"
			"Server: "HEADER_SERVER_NAME"\r\n" <<
			(contentbytes > 0 ? pfc::string8("Content-length: ") <<
				(end - begin + 1) << "\r\n" : "") <<
			"Content-type: audio/" << (lame != NULL ? "mpeg" : "wav") << "\r\n" <<
			(contentbytes > 0 ? "Accept-ranges: bytes\r\n" : "") <<
			(sendrange && contentbytes > 0 ? pfc::string8("Content-range: bytes ") <<
				begin << "-" << end << "/" << contentbytes << "\r\n" : "") <<
			"\r\n";
		mg_write(conn, header.get_ptr(), header.get_length());

		// prepare data
		if (lame != NULL) {
			lame_set_num_channels(lame, chunk.get_channel_count());
			lame_set_in_samplerate(lame, chunk.get_sample_rate());
			if (lame_init_params(lame) < 0) {
				throw std::exception("lame init failed");
			}
		}
		else if (wavbit == 8 || wavbit == 16 || wavbit == 24) {
			WAVE_FORMAT_HEADER wf = create_wave_header(chunk.get_channel_count(),
				chunk.get_sample_rate(), trackbytes, wavbit);
			sended = send_data_range(conn, current, (char *)&wf, sizeof(WAVE_FORMAT_HEADER), begin, end);
		}
		else {
			throw std::exception("unsupported encoding");
		}

		// do send
		do {
			// convert to mp3
			if (lame != NULL) {
				buf.set_size((t_size)1.25*chunk.get_sample_count() + 7200);
				int size = lame_encode_buffer_interleaved_ieee_float(lame, chunk.get_data(), chunk.get_sample_count(),
					(unsigned char *)buf.get_ptr(), buf.get_size());
				if (size < 0) {
					throw std::exception(pfc::string8("lame encode error: ") << size);
				}
				else {
					buf.set_size(size);
				}
			}
			// convert to wav
			else {
				proc->run(chunk, buf, wavbit, wavbit, false, 1.0);
			}

			// send data
			if (buf.get_size() > 0) {
				sended = send_data_range(conn, current, (char *)buf.get_ptr(), buf.get_size(), begin, end);
			}
		} while(ih.run(chunk, cb) && (trackbytes <= 0 || current < end + 1));

		// flush lame buffer for mp3
		if (lame != NULL && buf.get_size() > 0) {
			if (buf.get_size() < 7200) { // !! MUST check buffer length
				buf.set_size(7200);
			}
			int size = lame_encode_flush(lame, (unsigned char *)buf.get_ptr(), buf.get_size());
			if (size > 0) {
				sended = send_data_range(conn, current, (char *)buf.get_ptr(), size, begin, end);
			}
		}
		FOO_LOG << "stream[" << ri->remote_port << "]: " << sended << " bytes sent";
	}
	catch (std::exception &e) {
		FOO_LOG << "stream[" << ri->remote_port << "]: error: " << e.what();
	}
	ih.close();
	return current;
}

static int lsp_stream_file(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	mg_send_file(conn, fpath);
	lua_pushinteger(L, 0);
	return 1;
}

static int lsp_stream_wav(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	int subsong = luaL_checkinteger(L, 2);
	int seek = luaL_checkinteger(L, 3);
	int wavbit = lua_gettop(L) >=4 ? luaL_checkinteger(L, 4) : 16;
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	lua_pushinteger(L, get_track(conn, fpath, subsong, seek, NULL, wavbit));
	return 1;
}

static int lsp_stream_mp3(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	int subsong = luaL_checkinteger(L, 2);
	int seek = luaL_checkinteger(L, 3);
	int quality = lua_gettop(L) >=4 ? luaL_checkinteger(L, 4) : 2;
	int bitrate = lua_gettop(L) >=5 ? luaL_checkinteger(L, 5) : 320;
	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	lame_t lame = lame_init();
	if (lame == NULL) {
		lua_pushinteger(L, -1);
	}
	else {
		if (quality >= 0) {
			lame_set_VBR(lame, vbr_default);
			lame_set_VBR_quality(lame, (float)quality);
			lame_set_VBR_mean_bitrate_kbps(lame, bitrate);
		}
		lua_pushinteger(L, get_track(conn, fpath, subsong, seek, lame, 0));
		lame_close(lame);
	}
	return 1;
}

static int lsp_proxy_url(lua_State *L) {
	const char *host = luaL_checkstring(L, 1);
	const int port = luaL_checkinteger(L, 2);
	const char *path = luaL_checkstring(L, 3);

	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));

	char szBuf[4096];
	mg_connection* req = mg_download(host, port, 0,
		szBuf, sizeof(szBuf),
		"GET %s HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"\r\n",
		path, host, port);

	int recvd = 0;
	if (req != NULL) {
		const struct mg_request_info *ri = mg_get_request_info(req);
		int size = sprintf_s(szBuf, sizeof(szBuf), "%s %s %s\r\n", ri->request_method, ri->uri, ri->http_version);
		mg_write(conn, szBuf, size);

		for (int i = 0; i < ri->num_headers; i++) {
			const mg_request_info::mg_header* header = ri->http_headers + i;
			const int size = sprintf_s(szBuf, sizeof(szBuf), "%s: %s\r\n", header->name, header->value);
			mg_write(conn, szBuf, size);
		}

		mg_write(conn, "\r\n", 2);

		while ((size = mg_read(req, szBuf, sizeof(szBuf))) > 0) {
			recvd += size;
			if (mg_write(conn, szBuf, size) != size) {
				size = -1;
				break;
			}
		}
		FOO_LOG << "proxy " << (size < 0 ? "error" : "ok") << ": " << recvd << "bytes proxied";
		mg_close_connection(req);
	}
	else {
		FOO_LOG << "proxy error: " << szBuf;
	}

	lua_pushinteger(L, recvd);
	return 1;
}

static int get_res_content_length(mg_connection* req) {
	int length = -1;
	const struct mg_request_info *ri = mg_get_request_info(req);
	for (int i = 0; i < ri->num_headers; i++) {
		const mg_request_info::mg_header* header = ri->http_headers + i;
		if (stricmp(header->name, "Content-length") == 0) {
			length = atoi(header->value);
		}
	}
	return length;
}

static int archive_open_cb(struct arv::archive *a, void *client_data) {
	return ARCHIVE_OK;
}

static SSIZE_T archive_write_cb(struct arv::archive *a, void *client_data,
	const void *buff, size_t length) {
	mg_connection *conn = (mg_connection *)client_data;
	return mg_write(conn, buff, length);
}

static int archive_close_cb(struct arv::archive *a, void *client_data) {
	return ARCHIVE_OK;
}

static int lsp_zip_urls(lua_State *L) {
	char szBuf[4096];
	bool failed = false;

	struct mg_connection *conn = (mg_connection *)lua_touserdata(L, lua_upvalueindex(1));
	c_initquit &iq = g_init.get_static_instance();
	const char* host = "127.0.0.1";
	const int port = iq.getListenPort();

	const char *file_name = luaL_checkstring(L, 1);
	mg_printf(conn, "HTTP/1.0 200 OK\r\n"
		"Content-type: application/zip\r\n"
		"Content-Disposition: attachment; filename=\"%s\"\r\n"
		"\r\n", file_name);

	struct arv::archive* a = arv::archive_write_new();
	arv::archive_write_set_format_zip(a);
	arv::archive_write_open(a, conn, archive_open_cb, archive_write_cb, archive_close_cb);

	// http://stackoverflow.com/questions/6137684/iterate-through-lua-table
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		lua_pushvalue(L, -2);
        const char *fname = lua_tostring(L, -1);
        const char *path = lua_tostring(L, -2);

		if (!failed) {
			FOO_LOG << "requesting url: " << fname << " -> " << path;

			mg_connection* req = mg_download(host, port, 0,
				szBuf, sizeof(szBuf),
				"GET %s HTTP/1.0\r\n"
				"Host: %s:%d\r\n"
				"\r\n",
				path, host, port);

			if (req != NULL) {
				struct arv::archive_entry* entry = arv::archive_entry_new();
				arv::archive_entry_set_pathname(entry, fname);
				arv::archive_entry_set_filetype(entry, AE_IFREG);
				arv::archive_entry_set_perm(entry, 0644);
				arv::archive_write_header(a, entry);

				int size;
				while ((size = mg_read(req, szBuf, sizeof(szBuf))) > 0) {
					arv::archive_write_data(a, szBuf, size);
				}

				arv::archive_write_finish_entry(a);
				arv::archive_entry_free(entry);
				mg_close_connection(req);
			}
			else {
				FOO_LOG << "download error: " << szBuf;
				failed = true;
			}
		}
		else {
			FOO_LOG << "ignoring url: " << fname << " -> " << path;
		}

		lua_pop(L, 2);
	}

	arv::archive_write_close(a);
	arv::archive_write_free(a);

	return 0;
}

static const struct luaL_Reg fb_stream[] = {
	{"stream_albumart", lsp_stream_albumart},
	{"extract_albumart", lsp_extract_albumart},
	{"stream_file", lsp_stream_file},
	{"stream_wav", lsp_stream_wav},
	{"stream_mp3", lsp_stream_mp3},
	{"proxy_url", lsp_proxy_url},
	{"zip_urls", lsp_zip_urls},
	{NULL, NULL}
};

static int lsp_log(lua_State *L) {
	const char *str;
	size_t size;
	pfc::string8 msg;
	int num_args = lua_gettop(L);
	for (int i = 1; i <= num_args; i++) {
		if (lua_isstring(L, i)) {
			str = lua_tolstring(L, i, &size);
			msg.add_string(str, size);
		}
	}
	FOO_LOG << msg;
	return 0;
}

static int lsp_exec(lua_State *L) {
	WCHAR wcmd[1024], wpara[1024];
	const char *cmd = luaL_checkstring(L, 1);
	int scmd = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, sizeof(wcmd));
	const char *para = luaL_checkstring(L, 2);
	int spara = MultiByteToWideChar(CP_UTF8, 0, para, -1, wpara, sizeof(wpara));

	SHELLEXECUTEINFO si = {0};
	si.cbSize = sizeof(SHELLEXECUTEINFO);
	si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	si.hwnd = NULL;
	si.lpVerb = NULL;
	si.lpFile = wcmd;
	si.lpParameters = wpara;
	si.nShow = SW_HIDE;
	si.hInstApp = NULL;
	ShellExecuteEx(&si);
	WaitForSingleObject(si.hProcess, INFINITE);

	DWORD dwExit = 0;
	GetExitCodeProcess(si.hProcess, &dwExit);
	lua_pushinteger(L, dwExit);
	return 1;
}

static int lsp_store(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	c_initquit *iq = &g_init.get_static_instance();
	if (lua_gettop(L) >= 2) {
		const char *val = luaL_checkstring(L, 2);
		if (lua_State *lua = iq->acquireLua()) {
			lua_pushstring(lua, val);
			lua_setglobal(lua, key);
			iq->releaseLua();
		}
	}
	else {
		const char *val = NULL;
		if (lua_State *lua = iq->acquireLua()) {
			lua_getglobal(lua, key);
			if (lua_isstring(lua, -1) &&
					(val = lua_tostring(lua, -1))) {
				lua_pushstring(L, val);
			}
			lua_pop(lua, 1);
			iq->releaseLua();
		}
		if (val != NULL)
			return 1;
	}
	return 0;
}

static int lsp_random(lua_State *L) {
	srand(GetTickCount());
	lua_pushinteger(L, rand());
	return 1;
}

static int lsp_md5(lua_State *L) {
	char buf[34] = {0};
	const char *src = luaL_checkstring(L, 1);
	mg_md5(buf, src, NULL);
	lua_pushstring(L, buf);
	return 1;
}

static int lsp_list_dir(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	pfc::ptrholder_t<uFindFile> find = uFindFirstFile(fpath);
	if (find.is_empty()) {
		return 0;
	}
	int i = 1;
	lua_newtable(L);
	do {
		lua_pushinteger(L, i);

		lua_newtable(L);
		lua_pushinteger(L, 1);
		lua_pushstring(L, find->GetFileName());
		lua_rawset(L, -3);
		lua_pushinteger(L, 2);
		lua_pushinteger(L, find->GetAttributes());
		lua_rawset(L, -3);

		lua_rawset(L, -3);
		i ++;
	} while(find->FindNext());
	return 1;
}

static int lsp_file_stat(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	pfc::ptrholder_t<uFindFile> find = uFindFirstFile(fpath);
	if (find.is_empty()) {
		return 0;
	}
	lua_newtable(L);
	reg_string(L, "name", find->GetFileName());
	reg_int(L, "attr", find->GetAttributes());
	reg_int(L, "size", (int)find->GetFileSize());
	reg_int(L, "created", (lua_Integer)FileTimeToSeconds(find->GetCreationTime()));
	reg_int(L, "accessed", (lua_Integer)FileTimeToSeconds(find->GetLastAccessTime()));
	reg_int(L, "modified", (lua_Integer)FileTimeToSeconds(find->GetLastWriteTime()));
	reg_int(L, "is_dir", find->IsDirectory());
	return 1;
}

static int lsp_file_exists(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	lua_pushboolean(L, uFileExists(fpath));
	return 1;
}

static int lsp_path_canonical(lua_State *L) {
	const char *fpath = luaL_checkstring(L, 1);
	WCHAR wbuf[1024], obuf[MAX_PATH]; char cbuf[1024];
	int wsz = MultiByteToWideChar(CP_UTF8, 0, fpath, -1, wbuf, sizeof(wbuf));
	if (PathCanonicalizeW(obuf, wbuf)) {
		int csz = WideCharToMultiByte(CP_UTF8, 0, obuf, wcslen(obuf), cbuf, sizeof(cbuf), NULL, false);
		if (csz > 0) {
			lua_pushlstring(L, cbuf, csz);
			return 1;
		}
	}
	return 0;
}

static int lsp_url_encode(lua_State *L) {
	const char *url = luaL_checkstring(L, 1);
	pfc::string8 encoded;
	pfc::urlEncode(encoded, url);
	lua_pushlstring(L, encoded.get_ptr(), encoded.get_length());
	return 1;
}

static int lsp_is_utf8(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	lua_pushboolean(L, pfc::is_valid_utf8(str));
	return 1;
}

static int lsp_utf8_len(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	t_size size = pfc::strlen_utf8(str);
	lua_pushinteger(L, size);
	return 1;
}

static int lsp_utf8_sub(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	// lua string starts at 1
	int ib = lua_gettop(L) >= 2 ? luaL_checkinteger(L, 2) : 0;
	int ie = lua_gettop(L) >= 3 ? luaL_checkinteger(L, 3) : 0;
	unsigned b = ib > 1 ? pfc::skip_utf8_chars(str, ib-1) : 0;
	if (ie > 1) {
		unsigned e = pfc::skip_utf8_chars(str, ie-1);
		lua_pushstring(L, pfc::string(str).subString(b, e-b+1).get_ptr());
	}
	else
		lua_pushstring(L, pfc::string(str).subString(b).get_ptr());
	return 1;
}

static int lsp_utf8_to_codepage(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	unsigned cp = luaL_checkinteger(L, 2);
	pfc::stringcvt::string_codepage_from_utf8 out = pfc::stringcvt::string_codepage_from_utf8(cp, str);
	lua_pushlstring(L, out.get_ptr(), out.length());
	return 1;
}

static int lsp_codepage_to_utf8(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	unsigned cp = luaL_checkinteger(L, 2);
	pfc::stringcvt::string_utf8_from_codepage out = pfc::stringcvt::string_utf8_from_codepage(cp, str);
	lua_pushlstring(L, out.get_ptr(), out.length());
	return 1;
}

static const struct luaL_Reg fb_util[] = {
	{"log", lsp_log},
	{"exec", lsp_exec},
	{"store", lsp_store},
	{"random", lsp_random},
	{"md5", lsp_md5},
	{"list_dir", lsp_list_dir},
	{"file_stat", lsp_file_stat},
	{"file_exists", lsp_file_exists},
	{"path_canonical", lsp_path_canonical},
	{"url_encode", lsp_url_encode},
	{"is_utf8", lsp_is_utf8},
	{"utf8_len", lsp_utf8_len},
	{"utf8_sub", lsp_utf8_sub},
	{"utf8_to_codepage", lsp_utf8_to_codepage},
	{"codepage_to_utf8", lsp_codepage_to_utf8},
	{NULL, NULL}
};