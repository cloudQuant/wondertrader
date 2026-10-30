import pathlib
import shlex
import subprocess

root = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build = root / 'build-cov'
out = pathlib.Path('/tmp/wt-i02-t4-002')

def commands(target):
    return subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()

def run(tokens, cwd, label):
    print('$ ' + ' '.join(shlex.quote(t) for t in tokens), flush=True)
    result = subprocess.run(tokens, cwd=cwd)
    print(f'{label}_exit={result.returncode}', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)

# Rebuild the production target outside build-cov.
prod_commands = commands('TraderFemas')
prod_compile = next(line for line in prod_commands if ' -c ' in line and line.endswith('/src/TraderFemas/TraderFemas.cpp'))
prod_tokens = shlex.split(prod_compile)
prod_obj = out / 'TraderFemas-production.o'
prod_tokens[prod_tokens.index('-o') + 1] = str(prod_obj)
prod_tokens[prod_tokens.index('-MF') + 1] = str(out / 'TraderFemas-production.d')
run(prod_tokens, build, 'production_compile')
prod_link_line = next(line for line in prod_commands if line.startswith(': && /usr/bin/c++') and ' -dynamiclib ' in line)
prod_link = shlex.split(prod_link_line[len(': && '):].rsplit(' && :', 1)[0])
old_obj = next(t for t in prod_link if t.endswith('TraderFemas.cpp.o'))
prod_link[prod_link.index(old_obj)] = str(prod_obj)
prod_link[prod_link.index('-o') + 1] = str(out / 'libTraderFemas.dylib')
run(prod_link, build, 'production_link')

# Recompile every source in the focused coverage target into /tmp.
test_commands = commands('CoverageTests_t_TraderFemas')
source_names = ['femas_trader_test.cpp', 'gate_placeholder_test.cpp', 'r93_traderFemas_api_test.cpp']
compiled = {}
for line in test_commands:
    if ' -c ' not in line:
        continue
    name = next((candidate for candidate in source_names if line.endswith(candidate)), None)
    if name is None and line.endswith('/src/TraderFemas/TraderFemas.cpp'):
        name = 'TraderFemas-coverage.cpp'
    if name is None or name in compiled:
        continue
    tokens = shlex.split(line)
    obj = out / (name.replace('.cpp', '') + '.o')
    dep = out / (name.replace('.cpp', '') + '.d')
    original_obj = tokens[tokens.index('-o') + 1]
    tokens[tokens.index('-o') + 1] = str(obj)
    tokens[tokens.index('-MF') + 1] = str(dep)
    run(tokens, build, name + '_compile')
    compiled[name] = (original_obj, obj)
required = set(source_names + ['TraderFemas-coverage.cpp'])
if set(compiled) != required:
    raise SystemExit(f'missing_compile_commands={sorted(required - set(compiled))}')

link_line = next(line for line in test_commands if line.startswith(': && /usr/bin/c++') and 'CoverageTests_t_TraderFemas' in line and ' -o ' in line)
link = shlex.split(link_line[len(': && '):].rsplit(' && :', 1)[0])
for original_obj, temp_obj in compiled.values():
    link[link.index(original_obj)] = str(temp_obj)
link[link.index('-o') + 1] = str(out / 'CoverageTests_t_TraderFemas')
run(link, build, 'test_link')
print(f'binary={out / "CoverageTests_t_TraderFemas"}', flush=True)
