"""The allowlist of tests/ia32/validity_exceptions.txt: the encodings where
dismount follows the Intel SDM and not ndisasm. Shared by fuzz_validity.py and
fuzz_lengths.py (the format is documented in the allowlist file)."""

import os
import re

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ALLOW = os.path.join(os.path.dirname(TOOLS_DIR), 'validity_exceptions.txt')


def load_allow(path=DEFAULT_ALLOW):
    """Returns the rules: (bits, categories, compiled regex)."""
    rules = []
    if not os.path.exists(path):
        return rules
    with open(path, encoding='utf-8') as f:
        for raw in f:
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 3:
                raise ValueError('bad allowlist line: %s' % raw.rstrip())
            rules.append((fields[0], fields[1].split(','), re.compile(fields[2].lower())))
    return rules


def allowed(rules, bits, category, hexs):
    """True when a rule of this mode and category fully matches the bytes (lower
    case hex, at least 16 bytes: pad with zero bytes)."""
    for b, cats, regex in rules:
        if (b == 'all' or str(bits) in b.split(',')) and \
                ('*' in cats or category in cats) and regex.fullmatch(hexs):
            return True
    return False
