import pathlib
import shlex
import subprocess

root = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build = root / 'build-cov'
out = pathlib.Path('/tmp/wt-i02-t3-004')

def commands(target):
    return subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()

def run(tokens, cwd, label):
    print('$ ' + ' '.join(shlex.quote(t) for t in tokens), flush=True)
    result = subprocess.run(tokens, cwd=cwd)
    print(f'{label}_exit={result.returncode}', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)

# Compile and link the production ParserFemas shared-library target into /tmp.
prod_commands = commands('ParserFemas')
prod_compile = next(line for line in prod_commands if ' -c ' in line and line.endswith('/src/ParserFemas/ParserFemas.cpp'))
prod_tokens = shlex.split(prod_compile)
prod_obj = out / 'ParserFemas-production.o'
prod_tokens[prod_tokens.index('-o') + 1] = str(prod_obj)
prod_tokens[prod_tokens.index('-MF') + 1] = str(out / 'ParserFemas-production.d')
run(prod_tokens, build, 'production_compile')
prod_link_line = next(line for line in prod_commands if line.startswith(': && /usr/bin/c++') and ' -dynamiclib ' in line)
prod_link = shlex.split(prod_link_line[len(': && '):].rsplit(' && :', 1)[0])
old_obj = next(t for t in prod_link if t.endswith('ParserFemas.cpp.o'))
prod_link[prod_link.index(old_obj)] = str(prod_obj)
prod_link[prod_link.index('-o') + 1] = str(out / 'libParserFemas.dylib')
run(prod_link, build, 'production_link')

# Compile the exact test translation units and production source used by the focused GTest target.
test_commands = commands('CoverageTests_t_ParserFemas')
source_names = ['femas_parser_callbacks_test.cpp', 'gate_placeholder_test.cpp', 'r105_parserfemas_api_test.cpp']
compiled = {}
for line in test_commands:
    if ' -c ' not in line:
        continue
    name = next((candidate for candidate in source_names if line.endswith(candidate)), None)
    if name is None and line.endswith('/src/ParserFemas/ParserFemas.cpp'):
        name = 'ParserFemas-coverage.cpp'
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
required = set(source_names + ['ParserFemas-coverage.cpp'])
if set(compiled) != required:
    raise SystemExit(f'missing_compile_commands={sorted(required - set(compiled))}')

link_line = next(line for line in test_commands if line.startswith(': && /usr/bin/c++') and 'CoverageTests_t_ParserFemas' in line and ' -o ' in line)
link = shlex.split(link_line[len(': && '):].rsplit(' && :', 1)[0])
for original_obj, temp_obj in compiled.values():
    link[link.index(original_obj)] = str(temp_obj)
link[link.index('-o') + 1] = str(out / 'CoverageTests_t_ParserFemas')
run(link, build, 'test_link')
print(f'binary={out / "CoverageTests_t_ParserFemas"}', flush=True)
