tera源码目录结构
----
##### common
    公共库的实现。
##### io
    Tabletserver的数据存储相关逻辑的实现。
##### leveldb
    key-value存储的实现，基于开源系统leveldb。
##### master
    Master的实现。
##### proto
    Master、Tabletserver和SDK三者之间的通信协议。
##### sample
    Tera使用样例。
##### sdk
    SDK的实现。
##### tabletnode
    Tabletserver的tablet管理相关逻辑的实现。
##### utils
    常用函数的实现，便于上层模块使用。
##### zk
    Tera对zookeeper的封装，主要实现选主、存活性检测等逻辑。
##### tera_flags.cc
    Tera所有flags的定义及默认值。
##### tera_main.cc  tera_entry.cc
    Tera的服务端主程序入口。
##### teracli_main.cc
    Tera的命令行工具的实现，用于表格创建、schema更新和执行管理命令。
    
