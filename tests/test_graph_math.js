// Проверка математики графика (лаб. 3): разбор формулы, границы, масштаб по Y.
// Запуск: node tests/test_graph_math.js   (код берётся прямо из desktop/ui/index.html)
const fs = require('fs');
const path = require('path');
const html = fs.readFileSync(path.join(__dirname, '..', 'desktop', 'ui', 'index.html'), 'utf8');
const code = html.match(/<script id="mathlib">([\s\S]*?)<\/script>/)[1];
const M = new Function(code + ';return MathLib;')();

let fails = 0;
const t = (name, ok, got) => { if (!ok) { fails++; console.log('FAIL ' + name, got === undefined ? '' : got); } };
const ev = (s, x) => M.compile(s).fn(x);
const close = (a, b) => Math.abs(a - b) < 1e-9;

t('sin(x)', close(ev('sin(x)', 1), Math.sin(1)));
t('cos(x)/x+x*x', close(ev('cos(x)/x+x*x', 2), Math.cos(2) / 2 + 4));
t('приоритет', ev('2+3*4', 0) === 14);
t('-x^2 = -(x^2)', ev('-x^2', 3) === -9);
t('2^3^2 = 512', ev('2^3^2', 0) === 512);
t('2**3', ev('2**3', 0) === 8);
t('неявное умножение', ev('2x', 5) === 10 && ev('2(x+1)', 2) === 6 && ev('x(x+1)', 2) === 6);
t('pi, e', close(ev('pi', 0), Math.PI) && close(ev('E', 0), Math.E));
t('pow, max', ev('pow(2,3)', 0) === 8 && ev('max(1;5;3)', 0) === 5);
t('регистр', close(ev('SIN(X)', 1), Math.sin(1)));
t('1/0 — не число', Number.isNaN(M.evalAt(M.compile('1/x').fn, 0)));
t('sqrt(-1) — не число', Number.isNaN(M.evalAt(M.compile('sqrt(x)').fn, -1)));

const bad = (s, re) => { try { M.compile(s); t('ошибка «' + s + '»', false, 'не бросило'); } catch (e) { t('ошибка «' + s + '»: ' + e.message, e instanceof M.ExprError && re.test(e.message), e.message); } };
bad('', /Введите/); bad('sin(x', /закрывающей/); bad('sin x', /нужны скобки/); bad('foo(x)', /Неизвестное имя/);
bad('2+', /обрывается/); bad('x)', /Лишняя/); bad('3 $ 4', /Недопустимый символ/); bad('sin(1,2)', /принимает/);
for (const s of ['alert(1)', 'constructor(x)', '__proto__', 'toString(x)']) bad(s, /Неизвестное имя/);

const pb = (s) => M.parseBound(s, 'Левая граница', 'left');
t('границы', pb('-3') === -3 && pb('−3') === -3 && pb('1,5') === 1.5 && close(pb('2*pi'), 2 * Math.PI));
t('граница с x', (() => { try { pb('x'); return false; } catch (e) { return e.field === 'left'; } })());

const N = 1000;
const rng = (s, a, b) => { const f = M.compile(s).fn, ys = new Float64Array(N + 1); for (let i = 0; i <= N; i++) ys[i] = M.evalAt(f, a + (b - a) * i / N); return M.yRange(ys); };
let r = rng('sin(x)', -3, 18); t('sin: диапазон', r.lo > -1.3 && r.lo < -1 && r.hi > 1 && r.hi < 1.3, r);
r = rng('tan(x)', -3, 18); t('tan: полюса не сплющивают график', r.hi - r.lo < 100, r);
r = rng('1/x', -3, 18); t('1/x: полюс не сплющивает график', r.hi - r.lo < 1000, r);
r = rng('x*x', -3, 18); t('x*x полностью виден', r.lo < 0 && r.hi > 324, r);
r = rng('5', -1, 1); t('константа', r.lo < 5 && r.hi > 5, r);
t('нет значений → null', rng('ln(x)', -5, -1) === null);
t('шаг сетки и деления', JSON.stringify(M.ticks(-3, 18, 5)) === '[0,5,10,15]' && JSON.stringify(M.ticks(0, 0.5, 0.1)) === '[0,0.1,0.2,0.3,0.4,0.5]');
t('fmt', M.fmt(-3) === '−3' && M.fmt(1.5) === '1,5' && M.fmt(0) === '0' && M.fmt(12000000) === '1,2e7');

const env = { a: 2, b: 3, c: 1 };
const pf = M.compile('a*sin(b*x)+c', 'expr', env).fn;
t('параметры a, b, c', close(pf(1), 2 * Math.sin(3) + 1));
env.a = 0; t('параметр меняется без повторного разбора', close(pf(1), 1));
t('без env «a» неизвестно', (() => { try { M.compile('a*x'); return false; } catch (e) { return /Неизвестное имя/.test(e.message); } })());

if (fails) { console.log('ПРОВАЛ: ' + fails); process.exit(1); }
console.log('OK: математика графика');
