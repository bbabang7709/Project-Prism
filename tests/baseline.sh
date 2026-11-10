#!/bin/sh
# 모듈 분리 전 원본(tests/reference/prism_orig.c)과 지금 빌드(./prism)의 출력을 비교한다.
#   - --check 글자 출력: 같아야 한다
#   - --shot PPM 이미지: 바이트 단위로 같아야 한다 (같은 컴파일러·플래그로 원본도 빌드)
#   - 실행 시간(ms) 줄은 비교에서 뺀다
# 사용: make test   또는   sh tests/baseline.sh
set -u
cd "$(dirname "$0")/.."

OUT=tests/out
REF="$OUT/prism_ref"
NEW=./prism
mkdir -p "$OUT/ref" "$OUT/new"

if [ ! -x "$NEW" ]; then echo "먼저 make 하세요"; exit 1; fi
gcc -Wall -Wextra -O2 -march=native -ffast-math -pthread -o "$REF" tests/reference/prism_orig.c -lm -pthread || exit 1

# 이름|인자
CASES='check|--check
check_solve|--check --solve
check_block|--check --solve --fp 5.5 5.5 0 0
shot_top|--shot @.ppm
shot_angle|--shot @.ppm 1.2 -0.6 15
shot_solve|--shot @.ppm --solve
shot_fp|--shot @.ppm --fp 2.5 8.5 1.5708 -0.35
shot_fp_solve|--shot @.ppm --fp 9.5 4.5 3.1416 -0.45 --solve
shot_fp_block|--shot @.ppm --fp 5.5 5.5 0 -0.3 --solve
shot_select|--shot @.ppm --select 2
shot_top_select|--shot @.ppm 2.4 -1.0 12 --select 4 --solve'

rm -f "$OUT/fail"
echo "$CASES" | while IFS='|' read -r name args; do
    for side in ref new; do
        bin=$REF; [ $side = new ] && bin=$NEW
        a=$(echo "$args" | sed "s#@#$OUT/$side/$name#")
        # shellcheck disable=SC2086
        $bin $a 2>&1 | sed 's/, [0-9.]* ms with/, X ms with/; s#/ref/#/X/#; s#/new/#/X/#' > "$OUT/$side/$name.txt"
    done
    ok=1
    cmp -s "$OUT/ref/$name.txt" "$OUT/new/$name.txt" || ok=0
    if [ -f "$OUT/ref/$name.ppm" ]; then cmp -s "$OUT/ref/$name.ppm" "$OUT/new/$name.ppm" || ok=0; fi
    if [ $ok = 1 ]; then echo "  같음   $name"; else echo "  다름   $name"; echo x >> "$OUT/fail"; fi
done

if [ -f "$OUT/fail" ]; then
    echo "실패: $(wc -l < "$OUT/fail")개 경우가 원본과 다릅니다 (tests/out/ref, tests/out/new 비교)"
    rm -f "$OUT/fail"
    exit 1
fi
echo "통과: 모든 경우가 원본과 같습니다"
