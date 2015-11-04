这是一个foobar2000的插件，因此你可能先需要一份 [foobar2000](http://www.foobar2000.org/download)

本项目只包含 dll 部分，lua 脚本和 html 页面在 [foo\_mg\_www](https://github.com/oxyflour/foo_mg_www)。
完整安装包请点击[这里](https://github.com/oxyflour/foo_mg/releases/download/0.22/foo_mg_v0.22.zip)下载

##主要功能
###web 服务器集成
这个插件集成了一个轻量级 http 服务器([mongoose](https://github.com/cesanta/mongoose))，
你可以在 foobar2000 目录下创建一个 mongoose.conf 的文件来配置这个服务器的主目录、端口、cgi路径等信息
（具体设定请参考 [这里](https://github.com/cesanta/mongoose/blob/52e3be5c58bf5671d0cc010e520395bc308326b4/UserManual.md)）

如果没有配置文件，那默认端口就是8080，默认主目录是插件目录 user-components 下的foo\_mg\www文件夹

###媒体数据库同步（database.h）
这个插件会尝试自动将 foobar2000的 媒体库导出到 foobar2000 目录下的 mgdatabase.db3 文件。
这个文件实际上是一个 sqlite3 数据库，你可以使用任何你喜欢的工具查看、修改或者添加额外的信息。
[foo\_mg\_www](https://github.com/oxyflour/foo_mg_www) 中的 lua 脚本通过读取这个数据库以 json 的形式返回媒体信息

第一次安装后启动 foobar2000 时，可能需要一些时间把媒体库中的项目同步到 mgdatabase.dat 中
（在我不是很快的硬盘上，导出10k+的项目需要几秒钟）。
之后如果仅仅只是零星添加一些项目，使用过程中不会感觉到异常

###lua接口支持（luacmd.h）
在服务器中运行的 lua 脚本（请注意是 .lua 文件，而不是 mongoose 自身支持的 .lp 文件）支持一系列的接口，提供以下功能：

1. 播放器控制，如播放、暂停（理论上支持更多的，抱歉我没有这方面的需求）
2. 媒体串流（wav和mp3），唱片封面导出
3. 其他方面的接口，如列目录，编码转换，导出json数据等

[foo\_mg\_www](https://github.com/oxyflour/foo_mg_www) 即是基于这些功能开发的 web app


##如何安装
请下载上面的安装包，然后打开 foobar2000 的菜单 File->Preference，选择左边的 Components，
将下载的 zip 文件拖进右边面板，重启 foobar2000 即可


###致谢
这个项目的编译需要以下项目的支持
* mongoose 4.1
* lua-cjson 2.1.0
* lame


###许可证
GPL v2
