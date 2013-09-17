这是一个foobar2000的插件，因此你可能先需要一份foobar2000（[http://www.foobar2000.org/download](http://www.foobar2000.org/download)）

##主要功能
###mongoose服务器集成
这个插件集成了一个mongoose的轻量级http服务器，你可以在foobar2000目录下创建一个mongoose.conf的文件来配置这个服务器的主目录、端口、cgi路径等信息（具体设定请参考 [这里](https://github.com/cesanta/mongoose/blob/master/docs/UserManual.md)）

如果没有配置文件，那默认端口就是8080，默认主目录是foobar2000目录下的www文件夹

###媒体数据库同步（通过database.h）
这个插件会尝试自动将foobar2000的媒体库导出到foobar2000目录下的mgdatabase.dat文件。这个文件实际上是一个sqlite3数据库，你可以使用任何你喜欢的工具查看或修改。当然，你也可以使用在mongoose中运行的php脚本或其他cgi程序来访问这个数据库

第一次安装后启动foobar2000时，如果你的媒体库中有大量项目，可能需要一些时间把数据同步到mgdatabase.dat中（在我不是很快的硬盘上，导出10k+的项目需要十几秒）。之后如果仅仅只是零星添加一些项目，使用过程中不会感觉到异常

###lua接口支持（参考luacmd.h）
在服务器中运行的lua脚本（请注意是.lua文件，而不是mongoose自身支持的.lp文件）支持一系列的接口，提供以下功能：

1. 播放器控制，如播放、暂停（理论上支持更多的，抱歉我没有这方面的需求）
2. 媒体串流（wav和mp3），唱片封面导出
3. 其他方面的接口，如列目录，编码转换，导出json数据等


##如何安装
这个简陋的插件暂时没有提供安装包的计划，请直接将 [foo_mg.dll](https://github.com/oxyflour/foo_mg/blob/master/latest_build/foo_mg.dll?raw=true) 复制到foobar2000目录\components文件夹下

