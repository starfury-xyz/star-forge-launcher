-- sfmod.lua: the STAR FORGE mod API (docs/mods.md, "Lua hooks").
-- The launcher registers this script first in uiresources.txt whenever a mod adds scripts; don't
-- edit or ship it. Lua 5.1 (the game runs LuaJIT 2.1).
--
--   local M = SF.mod( "my-mod" )
--   M:wrap( UI.SomeWnd, "Update", function( orig, self, ... ) ... return orig( self, ... ) end )
--   M:replace( UI.SomeWnd, "Layout", function( self ) ... end )
--   M:log( "hello %s", "world" )
--
-- Each hooked function has one base (the game's, or the last replacement) and the wraps of each mod
-- in the order they were added (later mods outside earlier ones). The function is rebuilt from
-- those every time a hook is added and stored back into the table with rawset, so the game sees
-- a plain function. Running a mod's script again (a UI reload) updates its hooks in place.

local VERSION = "1"

SF = SF or {}
local SF = SF
SF.version = VERSION
SF.hooks = SF.hooks or setmetatable( {}, { __mode = "k" } )	-- table -> name -> record
SF.mods = SF.mods or {}
SF.settings = SF.settings or {}
SF.conflicts = SF.conflicts or {}

local function say( prefix, fmt, ... )
	local ok, text = pcall( string.format, fmt, ... )
	if not ok then
		text = tostring( fmt )
	end
	text = prefix .. text
	if sys and sys.log then
		sys.log( "%s", text )
	else
		print( text )
	end
end

-- "UI.HudModulesWnd" for a table directly in UI or _G, else a generic name.
local function tableName( tbl )
	if type( UI ) == "table" then
		for k, v in pairs( UI ) do
			if v == tbl then
				return "UI." .. tostring( k )
			end
		end
	end
	for k, v in pairs( _G ) do
		if v == tbl then
			return tostring( k )
		end
	end
	return tostring( tbl )
end

local function noop()
end

local function rebuild( tbl, name, rec )
	local f = rec.base or noop
	for _, w in ipairs( rec.wraps ) do
		local inner, wrapper = f, w.fn
		f = function( ... )
			return wrapper( inner, ... )
		end
	end
	rec.installed = f
	rawset( tbl, name, f )
end

-- The record for tbl[name], picking up a new base when something other than SF changed it
-- (the game's script ran again).
local function record( M, tbl, name, what )
	if type( tbl ) ~= "table" then
		error( string.format( "%s:%s: not a table (%s)", M.id, what, tostring( tbl ) ), 3 )
	end
	if type( name ) ~= "string" then
		error( string.format( "%s:%s: function name must be a string", M.id, what ), 3 )
	end
	local byName = SF.hooks[ tbl ]
	if not byName then
		byName = {}
		SF.hooks[ tbl ] = byName
	end
	local rec = byName[ name ]
	local current = tbl[ name ]
	if not rec then
		rec = { title = tableName( tbl ) .. ":" .. name, base = current, wraps = {} }
		byName[ name ] = rec
	elseif current ~= rec.installed then
		rec.base = current
		rec.baseBy = nil
	end
	if rec.base ~= nil and type( rec.base ) ~= "function" then
		error( string.format( "%s:%s: %s is a %s, not a function", M.id, what, rec.title, type( rec.base ) ), 3 )
	end
	return rec
end

local Mod = {}
Mod.__index = Mod

-- fn( orig, ... ) runs instead of tbl[name]; call orig( ... ) to run what was there.
function Mod:wrap( tbl, name, fn )
	if type( fn ) ~= "function" then
		error( self.id .. ":wrap: fn must be a function", 2 )
	end
	local rec = record( self, tbl, name, "wrap" )
	if rec.base == nil then
		self:log( "wrap: %s doesn't exist (yet); wrapping nothing", rec.title )
	end
	for _, w in ipairs( rec.wraps ) do
		if w.mod == self.id then
			w.fn = fn
			rebuild( tbl, name, rec )
			return
		end
	end
	table.insert( rec.wraps, { mod = self.id, fn = fn } )
	rebuild( tbl, name, rec )
end

-- fn becomes the base function; wraps (from any mod) still run around it.
function Mod:replace( tbl, name, fn )
	if type( fn ) ~= "function" then
		error( self.id .. ":replace: fn must be a function", 2 )
	end
	local rec = record( self, tbl, name, "replace" )
	if rec.baseBy and rec.baseBy ~= self.id then
		local line = string.format( "%s replaced by %s and %s; %s wins", rec.title, rec.baseBy, self.id, self.id )
		table.insert( SF.conflicts, line )
		say( "[sfmod] conflict: ", "%s", line )
	end
	rec.base = fn
	rec.baseBy = self.id
	rebuild( tbl, name, rec )
end

function Mod:log( fmt, ... )
	say( "[" .. self.id .. "] ", fmt, ... )
end

-- The handle for mod `id`; the same table every time it's asked for.
function SF.mod( id )
	if type( id ) ~= "string" or id == "" then
		error( "SF.mod: give your mod's id", 2 )
	end
	local M = SF.mods[ id ]
	if not M then
		SF.settings[ id ] = SF.settings[ id ] or {}
		M = setmetatable( { id = id, settings = SF.settings[ id ] }, Mod )
		SF.mods[ id ] = M
	end
	return M
end

say( "[sfmod] ", "API %s ready", VERSION )
