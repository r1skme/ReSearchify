#!/usr/bin/env python3
"""Check exact AND-anchor output and budgets through multisegment update churn."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def request(executable, index, command, **fields):
    process = subprocess.run([str(executable), 'request', '--stdin'],
                             input=json.dumps({'command': command, 'index': str(index),
                                               'memory_mb': 128, **fields}) + '\n',
                             text=True, capture_output=True, timeout=30)
    response = json.loads(process.stdout)
    if response['status'] == 'error':
        assert process.returncode != 0
    else:
        assert process.returncode == 0 and response['execution_complete'], response
        assert not response.get('output_truncated', False), response
    return response


def compare(before, after, context):
    compared = {}
    for field in ('status', 'results', 'total', 'has_more', 'next_cursor', 'snapshot',
                  'position_units', 'execution_complete'):
        assert before.get(field) == after.get(field), (context, field, before, after)
        compared[field] = before.get(field)
    if before['status'] == 'error':
        assert before['error']['code'] == after['error']['code'], (context, before, after)
        compared['error_code'] = before['error']['code']
    else:
        for field in ('candidates', 'scanned_bytes'):
            assert before['metrics'][field] == after['metrics'][field], (context, field, before, after)
            compared[field] = before['metrics'][field]
    return {'context': context, 'equal': True, 'result': compared}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', '--control', dest='control', type=Path, required=True)
    parser.add_argument('--after', '--experiment', dest='experiment', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    control, experiment = args.control.resolve(), args.experiment.resolve()
    checks = []
    identity = lambda binary: hashlib.sha256(binary.read_bytes()).hexdigest()
    identities = {'before': identity(control), 'after': identity(experiment)}
    with tempfile.TemporaryDirectory(prefix='rs-anchor-equivalence-') as temporary:
        base = Path(temporary)
        for profile in ('compact', 'fast'):
            source = base / profile / 'source'
            source.mkdir(parents=True)
            index = base / profile / 'index'
            for number in range(768):
                terms = []
                if number % 2 == 0:
                    terms.append('alpha')
                if number % 3 != 0:
                    terms.append('beta')
                if number % 5 != 0:
                    terms.append('gamma')
                content = ' '.join(terms) + '\n'
                content += f'common item_{number}\n'
                content += 'return common;\n' * (1 + number % 5)
                if number % 7 == 0:
                    content += 'alpha beta gamma\r\n'
                (source / f'file_{number:04}.cpp').write_text(content, encoding='utf-8')
            # No tokens at all: each trigram frequency is 1 > token count 0.
            # All required grams have df > 32 so this exercises AND membership.
            for number in range(384):
                punctuation = ('!@#$\n' if number % 2 == 0 else '')
                punctuation += '#$%^\n' if number % 3 == 0 else ''
                punctuation += '!@#$%^\n' if number % 6 == 0 else ''
                (source / f'punct_{number:04}.cpp').write_text(punctuation, encoding='utf-8')
            response = request(control, index, 'create', root=str(source), profile=profile,
                               threads=2, segment_documents=64)
            assert response['status'] == 'ok', response
            queries = ['alpha AND beta', 'alpha AND beta AND gamma',
                       'alpha AND (beta OR gamma)', '(alpha AND beta) OR gamma',
                       '(alpha AND beta) OR (gamma AND common)',
                       'alpha AND NOT beta', 'alpha AND beta AND NOT gamma',
                       '"alpha beta"', '"alpha beta gamma"', '"beta gamma"',
                       '"alpha beta" AND common', '"alpha beta" OR gamma',
                       '"!@#$%^"', '"!@#$" AND "#$%^"',
                       '"!@#$%^" OR "alpha beta"', '"!@#$%^" AND NOT common',
                       'alpha AND alpha AND beta', 'alpha AND absent_token']
            for phase in range(3):
                if phase == 1:
                    names = []
                    for number in range(0, 96):
                        name = f'file_{number:04}.cpp'
                        (source / name).write_text('beta gamma replacement\n', encoding='utf-8')
                        names.append(name)
                    for number in range(0, 48):
                        name = f'punct_{number:04}.cpp'
                        (source / name).write_text('#$%^\n', encoding='utf-8')
                        names.append(name)
                    response = request(control, index, 'update', changed_files=names,
                                       threads=2, segment_documents=32)
                    assert response['status'] == 'ok', response
                if phase == 2:
                    names = []
                    for prefix, first, last in (('file', 96, 240), ('punct', 48, 96)):
                        for number in range(first, last):
                            name = f'{prefix}_{number:04}.cpp'
                            (source / name).unlink()
                            names.append(name)
                    response = request(control, index, 'update', changed_files=names,
                                       threads=2, segment_documents=32)
                    assert response['status'] == 'ok', response
                for query in queries:
                    for options in ({'limit': 8}, {'limit': 8, 'sort': 'path'},
                                    {'limit': 0, 'snippets': 0},
                                    {'limit': 8, 'snippets': 0, 'threads': 4},
                                    {'limit': 8, 'path': ['file_00*.cpp']}):
                        before = request(control, index, 'find', query=query, **options)
                        after = request(experiment, index, 'find', query=query, **options)
                        checks.append(compare(before, after, (profile, phase, query, options)))
                    measured = request(control, index, 'find', query=query, limit=0, snippets=0)
                    for field, maximum in (('max_candidates', measured['metrics']['candidates']),
                                           ('max_scan_bytes', measured['metrics']['scanned_bytes'])):
                        if maximum:
                            for budget in (maximum, maximum - 1):
                                if budget == 0:
                                    continue
                                options = {'limit': 0, 'snippets': 0, field: budget}
                                before = request(control, index, 'find', query=query, **options)
                                after = request(experiment, index, 'find', query=query, **options)
                                checks.append(compare(before, after, (profile, phase, query, options)))
                    if phase == 0 and query == '"!@#$%^"':
                        assert not measured['results']
                        assert measured['total'] == 64, measured
    assert identity(control) == identities['before'], 'before binary changed during comparison'
    assert identity(experiment) == identities['after'], 'after binary changed during comparison'
    report = {'status': 'passed', 'comparisons': len(checks),
              'scope': 'same compact/fast indexes; exact output and candidate/scan budgets; updates/deletions; zero-token trigrams',
              'binaries': {'before': {'path': str(control), 'sha256': identities['before']},
                           'after': {'path': str(experiment), 'sha256': identities['after']}}}
    if args.output:
        args.output.write_text(json.dumps({**report, 'cases': checks}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
