// Stand-in for a header set whose patch number looks new enough but that does not declare the 1.4 API: reading VK_HEADER_VERSION alone
// would accept it, compiling against it does not.
#pragma once
#define VK_HEADER_VERSION 363
#define VK_VERSION_1_3 1
