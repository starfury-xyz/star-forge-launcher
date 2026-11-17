-- Tests for src/mod/sfmod.lua. Run: luajit tests/sfmod_test.lua src/mod/sfmod.lua
local path = assert( arg[ 1 ], "usage: sfmod_test.lua <sfmod.lua>" )

local logged = {}
sys = { log = function( fmt, ... ) table.insert( logged, string.format( fmt, ... ) ) end }
UI = {}

local failures = 0
local function check( cond, what )
	if not cond then
		failures = failures + 1
		print( "FAIL: " .. what )
	end
end
local function eq( a, b, what )
	check( a == b, string.format( "%s: got %s, want %s", what, tostring( a ), tostring( b ) ) )
end

-- The game's script.
local function gameScript()
	UI.Wnd = {}
	function UI.Wnd:Name( x ) return "game(" .. x .. ")" end
	function UI.Wnd:Two() return 1, 2 end
end

gameScript()
dofile( path )
eq( SF.version, "1", "version" )
check( logged[ #logged ]:find( "API 1 ready" ), "ready logged" )

local A, B = SF.mod( "a" ), SF.mod( "b" )
eq( SF.mod( "a" ), A, "same handle" )

-- wraps nest in the order added; later outside
A:wrap( UI.Wnd, "Name", function( orig, self, x ) return "a[" .. orig( self, x ) .. "]" end )
B:wrap( UI.Wnd, "Name", function( orig, self, x ) return "b[" .. orig( self, x ) .. "]" end )
eq( UI.Wnd:Name( 1 ), "b[a[game(1)]]", "two wraps" )
check( rawget( UI.Wnd, "Name" ) ~= nil, "stored in the table itself" )
check( getmetatable( UI.Wnd ) == nil, "no metatable" )

-- a replacement registered after the wraps keeps them
B:replace( UI.Wnd, "Name", function( self, x ) return "B(" .. x .. ")" end )
eq( UI.Wnd:Name( 2 ), "b[a[B(2)]]", "replace keeps wraps" )

-- two mods replacing: the later wins, logged as a conflict
A:replace( UI.Wnd, "Name", function( self, x ) return "A(" .. x .. ")" end )
eq( UI.Wnd:Name( 3 ), "b[a[A(3)]]", "later replace wins" )
eq( #SF.conflicts, 1, "one conflict" )
check( SF.conflicts[ 1 ] == "UI.Wnd:Name replaced by b and a; a wins", "conflict text: " .. tostring( SF.conflicts[ 1 ] ) )

-- multiple returns pass through
A:wrap( UI.Wnd, "Two", function( orig, self ) return orig( self ) end )
local x, y = UI.Wnd:Two()
check( x == 1 and y == 2, "multiple returns" )

-- the same mod wrapping again (its script ran again) updates its wrap in place
A:wrap( UI.Wnd, "Name", function( orig, self, x ) return "a2[" .. orig( self, x ) .. "]" end )
eq( UI.Wnd:Name( 4 ), "b[a2[A(4)]]", "re-wrap in place" )

-- the game's script runs again on the same table: its function becomes the base, wraps stay
function UI.Wnd:Name( x ) return "game2(" .. x .. ")" end
A:wrap( UI.Wnd, "Name", function( orig, self, x ) return "a[" .. orig( self, x ) .. "]" end )
eq( UI.Wnd:Name( 5 ), "b[a[game2(5)]]", "game reload" )

-- a reload that makes a new table: hooks start over for it
gameScript()
dofile( path )
eq( SF.mod( "a" ), A, "handle survives sfmod reload" )
A:wrap( UI.Wnd, "Name", function( orig, self, x ) return "a[" .. orig( self, x ) .. "]" end )
eq( UI.Wnd:Name( 6 ), "a[game(6)]", "new table" )

-- wrapping a missing function wraps nothing and says so
A:wrap( UI.Wnd, "Missing", function( orig, self ) orig( self ); return "ok" end )
eq( UI.Wnd:Missing(), "ok", "missing base" )
check( logged[ #logged ]:find( "UI.Wnd:Missing doesn't exist" ), "missing logged" )

-- errors
check( not pcall( A.wrap, A, nil, "x", function() end ), "nil table" )
check( not pcall( A.wrap, A, UI.Wnd, "Name", nil ), "nil fn" )
UI.Wnd.value = 3
check( not pcall( A.wrap, A, UI.Wnd, "value", function() end ), "not a function" )
check( not pcall( SF.mod ), "SF.mod without id" )

-- logging
A:log( "n=%d", 7 )
eq( logged[ #logged ], "[a] n=7", "log" )
A:log( "bad %d", "x" )
eq( logged[ #logged ], "[a] bad %d", "log with bad args" )

-- settings
A.settings.k = 1
eq( SF.settings.a.k, 1, "settings" )

if failures > 0 then
	print( failures .. " failed" )
	os.exit( 1 )
end
print( "sfmod: all passed" )
