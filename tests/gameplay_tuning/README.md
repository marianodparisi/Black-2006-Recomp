# Native combat tuning

The fixtures compile the authored combat helpers against synthetic guest RAM,
with no retail code or assets. Configure this directory with CMake, build Release
or Debug, and run CTest for that configuration.

Infinite-health cases launch independent processes with the option Off, On and
invalid. They check all native gate-byte values, current-player identity and
type, the active split-screen shadow actor, stale shadow addresses, bad pointers,
unchanged guest RAM, and host floating-point state including
the diagnostic path. Generator guards reject missing, moved, changed or duplicate
instructions and verify repeat application is idempotent.

Private IDA 9.4 inspection of the supported executable identifies the native
player gate at actor +8F2 in 1ED1E0, 1ED2B0, 1ED5D0, 1ED700 and 1ED990. These
handlers cover ordinary incoming damage. The authored helper supplies a true
gate for the current player only when explicitly enabled; it never writes that
flag or health, and native/scripted invulnerability remains authoritative when
disabled. This does not bypass forced mission failure or revive an actor.

These source tests do not establish protection during gameplay. Runtime
acceptance requires matched Off/On exposure to ordinary incoming damage;
an attempted route that never loses health does not provide that evidence.
