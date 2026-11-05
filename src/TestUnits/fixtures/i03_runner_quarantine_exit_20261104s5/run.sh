#!/bin/bash
# run.sh <WtRunner-executable> <libWtDataStorage.dylib> <new-output-dir> [label] [cta|uft]
#   uft: drive WtUftRunner instead (I03-NEW-058) with a UFT config.
#
# I03-NEW-045 (iteration 03, s5-runners): drive the real WtRunner process entry
# with one V2 parser whose callback drain can never be proven (the local fake
# provider below reports wait_drained=ERROR, so card 9a quarantines it). The
# runner is started synchronously, sent SIGINT after a short delay, and its exit
# status is recorded. Before the fix the runner destructor called
# std::terminate (SIGABRT, status 134); after the fix the parser owner is
# retained until process exit with a diagnostic and the exit status reflects
# the run result. Local fake only: no vendor SDK, network or orders.
set -u
exe=${1:?WtRunner executable}; storage=${2:?libWtDataStorage.dylib}; out=${3:?new output dir}; label=${4:-run}; mode=${5:-cta}
fx=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$fx/../../.." && pwd)
case "$out" in /*) ;; *) echo 'output path must be absolute' >&2; exit 2;; esac
mkdir -p "$out"
provider="$out/libI03S5UncertainParser.dylib"
if [ ! -f "$provider" ]; then
  /usr/bin/c++ -std=c++17 -shared -fPIC -I"$src" -I/opt/homebrew/opt/boost/include -I/opt/homebrew/include \
    -DBOOST_BIND_GLOBAL_PLACEHOLDERS -DFMT_HEADER_ONLY -include "$src/Share/fmt_enum_compat.h" \
    -o "$provider" "$fx/uncertain_provider.cpp" > "$out/provider-build.log" 2>&1 || { echo "provider build failed" >&2; exit 2; }
fi
d="$out/$label"
rm -rf "$d"; mkdir -p "$d/parsers" "$d/data"
cp "$provider" "$d/parsers/"
cp "$storage" "$d/"
cat > "$d/sessions.json" <<'J'
{"ALLDAY":{"name":"AllDay","offset":0,"sections":[{"from":0,"to":0}]}}
J
cat > "$d/commodities.json" <<'J'
{"SHFE":{"rb":{"covermode":0,"pricemode":0,"category":1,"precision":0,"pricetick":1,"volscale":10,"name":"rb","exchg":"SHFE","session":"ALLDAY","holiday":"CHINA"}}}
J
cat > "$d/contracts.json" <<'J'
{"SHFE":{"rb2510":{"name":"rb2510","code":"rb2510","exchg":"SHFE","product":"rb","maxlimitqty":20,"maxmarketqty":10}}}
J
echo '{"CHINA":["20240101"]}' > "$d/holidays.json"
echo '{}' > "$d/fees.json"
cat > "$d/actpolicy.yaml" <<'Y'
default:
    order:
    -   action: close
        limit: 0
    -   action: open
        limit: 0
Y
cat > "$d/logcfg.yaml" <<'Y'
root:
    async: false
    level: info
    sinks:
    -   filename: Logs/runner_%s.log
        pattern: '[%Y.%m.%d %H:%M:%S - %-5l] %v'
        truncate: true
        type: daily_file_sink
Y
if [ "$mode" = uft ]; then
cat > "$d/config.yaml" <<'Y'
basefiles:
    session: sessions.json
    commodity: commodities.json
    contract: contracts.json
    holiday: holidays.json
env:
    product:
        session: ALLDAY
data:
    unused: 1
bspolicy: actpolicy.yaml
parsers:
-   active: true
    id: i03s5_uncertain
    module: I03S5UncertainParser
Y
else
cat > "$d/config.yaml" <<'Y'
basefiles:
    session: sessions.json
    commodity: commodities.json
    contract: contracts.json
    holiday: holidays.json
env:
    name: cta
    fees: fees.json
    product:
        session: ALLDAY
data:
    store:
        path: ./data/
bspolicy: actpolicy.yaml
parsers:
-   active: true
    id: i03s5_uncertain
    module: I03S5UncertainParser
traders: []
executers: []
Y
fi
(
  cd "$d" || exit 2
  LLVM_PROFILE_FILE="$d/%p.profraw" "$exe" -c config.yaml -l logcfg.yaml > stdout.log 2>&1 &
  pid=$!
  sleep 4
  # SIGINT is honoured only after run() installed its signal hooks; under heavy
  # load resend every 2 s. Bounded: the process must exit within ~24 s.
  for i in $(seq 1 120); do
    kill -0 "$pid" 2>/dev/null || break
    [ $((i % 20)) -eq 1 ] && kill -INT "$pid" 2>/dev/null
    sleep 0.1
  done
  for i in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid"; echo "timeout" > timeout; fi
  wait "$pid"; echo $? > exit
)
status=$(cat "$d/exit")
retained=$(grep -h -c "retained until process exit" "$d"/Logs/*.log 2>/dev/null | paste -sd+ - | bc 2>/dev/null)
echo "$label exit=$status retained_log_lines=${retained:-0} $( [ -f "$d/timeout" ] && echo TIMEOUT)"
