using System.Runtime.CompilerServices;

// 单元测试需要直接调用导入命名、重名处理、替换原语等内部实现；其他程序集只能使用公开 API。
[assembly: InternalsVisibleTo("ReplayClient.Tests")]
