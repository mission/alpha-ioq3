/*
Woot lua scripting for ioq3

these are the wrapper functions
*/

#include "server.h"

int LuaPrint ( lua_State *L ) {
	Com_Printf(va("%s", lua_tostring(L, 1)));
	return 1;
}

int LuaCbuf_AddText ( lua_State *L ) {
	Cbuf_AddText(va("%s", lua_tostring(L, 1)));
	return 1;
}

int LuaCbuf_ExecuteText ( lua_State *L ) {
	Cbuf_ExecuteText( EXEC_NOW, va("%s", lua_tostring(L, 1)));
	return 1;
}

 

void SV_startLua(void) {
	if (LS_running) {
		Com_Printf("lua is already running!\n");
		return;
	}
	LS_running = qfalse;
	Com_Printf("Starting Lua...\n");
	if (sv_luaonstartup->integer > 0) {
		LS = lua_open();
		luaL_openlibs(LS);
		LS_running = qtrue;
		static const luaL_Reg FUNCS [] = {
			{"Com_Printf", LuaPrint},
			{"Cbuf_AddText", LuaCbuf_AddText},
			{"Cbuf_ExecuteText", LuaCbuf_ExecuteText},
			{NULL,NULL}
		};	
		luaL_register(LS, "LC", FUNCS);
	}
	if (LS_running) {
		Com_Printf("Lua Started!\n");
	} else {
		Com_Printf("Lua failed to start!\n");
	}
}

void SV_stopLua(void) {
	if (LS_running) {
		lua_close(LS);
		LS_running = qfalse;
		Com_Printf("Lua Stopped!\n");
	} else {
		Com_Printf("lua is already stopped!\n");
	}
}