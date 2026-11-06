/*
** Engine settings for Lua. lua.h includes this file through LUA_USER_H while the engine builds Lua
** (cmake/GameEngineDependencies.cmake); nothing else includes it.
*/

/*
** A fixed seed for string hashing. Lua otherwise mixes the time and addresses into it, so the order `pairs` visits a
** table in, and with it what a script does, would change from run to run. Scripts are the project's own code, never
** text from the network, so the hash flooding a random seed defends against does not arise.
*/
#define luai_makeseed(L) ((void)(L), 0x2d1e5a3bu)
