"""C string literals for the generated rows.

MSVC limits a string literal to 16380 characters (C2026), so a long text is
written as adjacent literals ("..." "..."), which the compiler joins. The
pieces end after a space, so a token is never split.
"""

MAXIMUM_PIECE = 2000


def literal(text):
    """Returns 'text' (which has no quote or backslash) as C string literals"""
    pieces = []
    while len(text) > MAXIMUM_PIECE:
        cut = text.rfind(' ', 0, MAXIMUM_PIECE)
        cut = MAXIMUM_PIECE if cut < 0 else cut + 1
        pieces.append(text[:cut])
        text = text[cut:]
    pieces.append(text)
    return ' '.join('"%s"' % piece for piece in pieces)
