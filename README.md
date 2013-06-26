This plugin runs a mongoose HTTP server, with the following features:
1. It will synchronize foobar2000 media library to the sqlite3 database (mgdatabase.dat). If you run foobar2000 with this plugin for the first time, it might take a few seconds to dump the whole library (less than 10 seconds dumping 10k tracks for me)
2. For lua scripts, some special interface are offered, including
a) A few control methods (play , etc), may add playlist interface in the future
b) Interface to stream albumart, files or transcoded tracks (wav and mp3, and mp3 powered by l.a.m.e)
c) Utilities like string encoding conversion and json helpers (powered by lua-cjson)

Requires these following libraries to compile:
1. mongoose 3.7 (with lua and sqlite3 support)
2. lua 5.2.1
3. lame 3.99.5
4. lua cjson 2.1.0
Many thanks to these projects!
