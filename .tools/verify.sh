#!/bin/bash
# 把 .refimpl 里的参考实现叠到骨架上，验证「全部 102 个用例能通过」。
#
# 它和 fast.sh 的区别只有一点：fast.sh 是日常迭代（只打印测试尾巴），
# verify.sh 是**验收**（编译零 warning、退出码必须是 0、失败就给完整日志路径）。
# 改完测试之后跑这个，确认「测试本身写得对」，再去跑骨架看红榜。
#
# 用法：
#   bash .tools/verify.sh            本地 /tmp 编译并跑全量
#   bash .tools/verify.sh --keep     保留 /tmp/lsm_verify 方便看现场
#   LSM_HOST=me@server bash .tools/verify.sh   在远端编译
set -euo pipefail

HOST=${LSM_HOST:-}
WORK=/tmp/lsm_verify
KEEP=${1:-}

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 两种布局都支持：课程站点仓库（骨架在 code/ 下）和学生 clone 下来的仓库（根就是骨架）。
if [[ -d "$ROOT/code" ]]; then SKEL="$ROOT/code"; else SKEL="$ROOT"; fi
IMPL="$ROOT/.refimpl"

echo "[1/4] 把骨架复制到 $WORK，再叠上参考实现"
rm -rf "$WORK"
cp -r "$SKEL/." "$WORK"
rm -rf "$WORK/build"
cp "$IMPL"/src/*.cc "$WORK/src/"
cp "$IMPL"/include/lsm/*.h "$WORK/include/lsm/"
echo "      .cc: $(ls "$IMPL"/src/*.cc | wc -l) 个"

SCRIPT="set -e
  cd $WORK
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug > /tmp/verify_cfg.log 2>&1
  cmake --build build -j4 > /tmp/verify_make.log 2>&1
  echo '[3/4] 编译通过；warning 条数:' \$(grep -c 'warning' /tmp/verify_make.log || true)
  echo '[4/4] 运行测试'
  ./build/lsm_tests 2>&1 | tail -3"

if [[ -n "$HOST" ]]; then
  echo "[2/4] 打包传到 $HOST"
  export MSYS_NO_PATHCONV=1
  tar czf /tmp/lsm_verify.tgz -C "$(dirname "$WORK")" "$(basename "$WORK")"
  scp -q /tmp/lsm_verify.tgz "$HOST:/tmp/"
  ssh "$HOST" "cd /tmp && rm -rf $(basename "$WORK") && tar xzf lsm_verify.tgz && rm -f lsm_verify.tgz"
  ssh "$HOST" "$SCRIPT"
else
  echo "[2/4] 本地编译"
  bash -c "$SCRIPT"
fi

if [[ "$KEEP" != "--keep" ]]; then
  rm -rf "$WORK"
fi
