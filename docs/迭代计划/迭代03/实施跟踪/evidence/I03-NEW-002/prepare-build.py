from pathlib import Path
src=Path('/private/tmp/wt-i03-r15-tags'); dst=Path('/private/tmp/wt-i03-new002')
for name in ('build-normal.sh','build-asan.sh'):
 s=(src/name).read_text().replace('/private/tmp/wt-i03-r15-tags','/private/tmp/wt-i03-new002')
 (dst/name).write_text(s)
