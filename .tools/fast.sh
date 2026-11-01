#!/bin/bash
# 把骨架（可叠参考实现）编译并跑测试。
#
# 两种模式：
#   MODE=ref  （默认）先把 .refimpl 里的参考实现叠上去 —— 验证「测试本身写得对」
#   MODE=skel        什么都不叠，直接编骨架 —— 验证「骨架能编译、红得符合预期」
#
# 为什么要分模式：仓库里的 src/ 是给学生填的骨架，39 个 TODO 都是占位实现。
# 直接跑测试必然大面积飘红，那不是 bug，是设计。想知道「填完应该全绿」，
# 就得用 ref 模式。
#
# 用法：
#   bash .tools/fast.sh                  参考实现，跑全部测试
#   bash .tools/fast.sh Table            参考实现，只跑名字含 Table 的用例
#   MODE=skel bash .tools/fast.sh        骨架，跑全部（看红成什么样）
#   TAILN=80 bash .tools/fast.sh         多看几行输出
#   LSM_HOST=me@server bash .tools/fast.sh   在远端编译（默认在本地 /tmp 里编）
set -euo pipefail

HOST=${LSM_HOST:-}
WORK=/tmp/lsm_fast
STAGE=/tmp/lsm_stage

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 两种布局都支持：课程站点仓库（骨架在 code/ 下）和学生 clone 下来的仓库（根就是骨架）。
if [[ -d "$ROOT/code" ]]; then SKEL="$ROOT/code"; else SKEL="$ROOT"; fi
IMPL="$ROOT/.refimpl"

FILTER=${1:-}

echo "[1/3] 暂存骨架（MODE=${MODE:-ref}）"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -r "$SKEL/." "$STAGE/"
rm -rf "$STAGE/build"
if [[ "${MODE:-ref}" == "ref" ]]; then
  cp "$IMPL"/src/*.cc "$STAGE/src/"
  cp "$IMPL"/include/lsm/*.h "$STAGE/include/lsm/"
else
  echo "      跳过参考实现，直接编骨架"
fi

# 编译 + 跑测试。放在一段字符串里，这样「本地」和「远端」两条路走同一份命令。
SCRIPT="set -e
  rm -rf $WORK && mkdir -p $WORK
  cp -r $STAGE/. $WORK/
  cd $WORK
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug > /tmp/fast_cfg.log 2>&1
  cmake --build build -j4 > /tmp/fast_make.log 2>&1 || { echo '--- 编译失败 ---'; grep -E 'error' /tmp/fast_make.log | head -30; exit 1; }
  echo '[2/3] 编译通过；warning 条数:' \$(grep -c 'warning' /tmp/fast_make.log || true)
  grep -E 'warning' /tmp/fast_make.log | head -20 || true
  echo '[3/3] 运行测试'
  ./build/lsm_tests ${FILTER} 2>&1 | tail -\${TAILN:-40}"

if [[ -n "$HOST" ]]; then
  echo "      打包传到 $HOST"
  export MSYS_NO_PATHCONV=1
  tar czf /tmp/lsm_fast.tgz -C "$STAGE" .
  scp -q /tmp/lsm_fast.tgz "$HOST:/tmp/"
  ssh "$HOST" "cd /tmp && rm -rf $STAGE && mkdir -p $STAGE && tar xzf lsm_fast.tgz -C $STAGE && rm -f lsm_fast.tgz"
  ssh "$HOST" "$SCRIPT"
else
  bash -c "$SCRIPT"
fi
