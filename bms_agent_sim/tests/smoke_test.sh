#!/usr/bin/env bash
# End-to-end test: starts the simulator and checks Web API, SNMP, traps and NTP.
# Needs: curl, python3, net-snmp tools (snmpget/snmpwalk/snmpset).
# The NTP part binds UDP 123 and is skipped when not run as root.
set -u
cd "$(dirname "$0")/.."
BIN=./build/bms_agent_sim
HTTP=18080; SNMP=11161; TRAP=11162
WORK=$(mktemp -d)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

fail=0
check() { if eval "$2"; then echo "ok   - $1"; else echo "FAIL - $1"; fail=1; fi; }

$BIN --http-port $HTTP --snmp-port $SNMP --data-dir "$WORK/data" --rollback-sec 2 >"$WORK/sim.log" 2>&1 &
PIDS+=($!)
python3 tests/trap_listener.py --port $TRAP >"$WORK/trap.log" 2>&1 &
PIDS+=($!)
if [ "$(id -u)" = 0 ]; then
  python3 tests/fake_ntp_server.py --port 123 >"$WORK/ntp.log" 2>&1 &
  PIDS+=($!)
fi
sleep 1

URL=http://localhost:$HTTP
TOKEN=$(curl -s -XPOST $URL/api/login -d '{"user":"admin","password":"admin"}' | python3 -c 'import sys,json;print(json.load(sys.stdin).get("token",""))')
AUTH="Authorization: Bearer $TOKEN"
api() { curl -s -X "$1" "$URL$2" -H "$AUTH" ${3:+-d "$3"}; }

check "login" '[ -n "$TOKEN" ]'
check "status requires login" 'curl -s $URL/api/status | grep -q "login required"'
check "status has 16 cells" '[ "$(api GET /api/status | python3 -c "import sys,json;print(len(json.load(sys.stdin)[\"cells\"]))")" = 16 ]'
check "web UI served" 'curl -s $URL/ | grep -q "<title>BMS Agent</title>"'

# ---- network
check "reject gateway outside subnet" 'api PUT /api/config/network "{\"ip\":\"10.0.0.5\"}" | grep -q "same subnet"'
check "apply static IP" 'api PUT /api/config/network "{\"ip\":\"192.168.1.50\"}" | grep -q "\"ok\":true"'
check "change pending" 'api GET /api/config/network | grep -q "\"pending_confirm\":true"'
sleep 3
check "rolled back after timeout" 'api GET /api/config/network | grep -q "\"ip\":\"192.168.1.100\""'
api PUT /api/config/network '{"ip":"192.168.1.60"}' >/dev/null
check "confirm keeps new IP" 'api POST /api/config/network/confirm | grep -q "\"ok\":true"'
sleep 3
check "confirmed IP survives" 'api GET /api/config/network | grep -q "\"ip\":\"192.168.1.60\""'

# ---- snmp
check "snmpget sysName" 'snmpget -v2c -c public -Ovq localhost:$SNMP 1.3.6.1.2.1.1.5.0 | grep -q bms-agent'
check "snmpwalk cell table" '[ "$(snmpwalk -v2c -c public -On localhost:$SNMP 1.3.6.1.4.1.99999.1.3.1.2 | wc -l)" = 16 ]'
check "snmpbulkwalk whole MIB" '[ "$(snmpbulkwalk -v2c -c public -On localhost:$SNMP 1.3.6.1.4.1.99999 | wc -l)" -gt 40 ]'
check "v1 get" 'snmpget -v1 -c public -Ovq localhost:$SNMP 1.3.6.1.4.1.99999.1.2.11.0 | grep -q 16'
check "wrong community ignored" '! snmpget -v2c -c nope -t 0.5 -r 0 localhost:$SNMP 1.3.6.1.2.1.1.5.0 >/dev/null 2>&1'
check "set threshold with rw" 'snmpset -v2c -c private localhost:$SNMP 1.3.6.1.4.1.99999.1.5.4.0 i 25 >/dev/null'
check "threshold visible in web" 'api GET /api/config/alarm | grep -q "\"soc_low\":25"'
check "set out of range rejected" 'snmpset -v2c -c private localhost:$SNMP 1.3.6.1.4.1.99999.1.5.4.0 i 500 2>&1 | grep -q wrongValue'
check "set with ro community rejected" 'snmpset -v2c -c public localhost:$SNMP 1.3.6.1.4.1.99999.1.5.4.0 i 30 2>&1 | grep -q noAccess'

# ---- alarms and traps
api PUT /api/config/snmp "{\"trap_targets\":[{\"host\":\"127.0.0.1\",\"port\":$TRAP}]}" >/dev/null
api PUT /api/sim '{"cell":3,"mv_offset":600}' >/dev/null
sleep 2
check "over-voltage alarm active" 'api GET /api/alarms | grep -q "Cell over-voltage"'
check "alarm row in SNMP" 'snmpget -v2c -c public -Ovq localhost:$SNMP 1.3.6.1.4.1.99999.1.4.1.3.1 | grep -q "cell 3"'
check "raised trap received" 'grep -q bmsAlarmRaised "$WORK/trap.log"'
api PUT /api/sim '{"reset_faults":true}' >/dev/null
sleep 2
check "cleared trap received" 'grep -q bmsAlarmCleared "$WORK/trap.log"'
api PUT /api/sim '{"comm_fail":true}' >/dev/null
sleep 2
check "comm lost alarm" 'snmpget -v2c -c public -Ovq localhost:$SNMP 1.3.6.1.4.1.99999.1.2.10.0 | grep -q 2'
api PUT /api/sim '{"comm_fail":false}' >/dev/null

# ---- time / ntp
check "manual time set" 'api PUT /api/system/time "{\"epoch_ms\":$(date +%s)000}" | grep -q "\"ok\":true"'
if [ "$(id -u)" = 0 ]; then
  api PUT /api/config/ntp '{"enable":true,"server1":"127.0.0.1","server2":""}' >/dev/null
  sleep 2
  check "ntp synced" 'api GET /api/config/ntp | grep -q "\"synced\":true"'
  check "ntp status in SNMP" 'snmpget -v2c -c public -Ovq localhost:$SNMP 1.3.6.1.4.1.99999.1.1.4.0 | grep -q 1'
else
  echo "skip - NTP test (needs root for UDP 123)"
fi

# ---- persistence / reboot
check "reboot" 'api POST /api/system/reboot | grep -q rebooting'
sleep 1
TOKEN=$(curl -s -XPOST $URL/api/login -d '{"user":"admin","password":"admin"}' | python3 -c 'import sys,json;print(json.load(sys.stdin).get("token",""))')
AUTH="Authorization: Bearer $TOKEN"
check "config persisted across reboot" 'api GET /api/config/alarm | grep -q "\"soc_low\":25"'

if [ $fail = 0 ]; then echo "ALL PASSED"; else echo "SOME CHECKS FAILED (log: $WORK/sim.log)"; cat "$WORK/sim.log"; fi
exit $fail
