"""The SME encodings which tools/learn_sme.py learned from objdump
(a64groups_sme_learned.py): the multi vector instructions of SME2, the lookup
tables, the 4 way outer products and the structured sparse ones."""
from a64dsl_sme import *
import a64groups_sme_learned


def groups():
    for mnemonic, mask, value, features, specs in a64groups_sme_learned.ROWS:
        add_row(mnemonic, mask, value, features, specs, '%08x/%08x' % (value, mask))
