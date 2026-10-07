"""Parse / write a compiled Warband mission_templates.txt."""

class Tok:
    def __init__(self, text):
        self.t = text.split()
        self.i = 0
    def next(self):
        v = self.t[self.i]; self.i += 1; return v
    def int(self):
        return int(self.next())

def read_block(tk):
    n = tk.int()
    out = []
    for _ in range(n):
        op = tk.int(); na = tk.int()
        out.append([op] + [tk.int() for _ in range(na)])
    return out

def parse(text):
    tk = Tok(text)
    assert tk.next() == "missionsfile" and tk.next() == "version" and tk.next() == "1"
    tpls = []
    for _ in range(tk.int()):
        t = {"id": tk.next(), "name": tk.next(), "flags": tk.int(), "type": tk.int(), "desc": tk.next()}
        groups = []
        for _ in range(tk.int()):
            g = [tk.int() for _ in range(5)]
            g.append([tk.int() for _ in range(tk.int())])
            groups.append(g)
        t["groups"] = groups
        trigs = []
        for _ in range(tk.int()):
            f = (tk.next(), tk.next(), tk.next())
            trigs.append([f, read_block(tk), read_block(tk)])
        t["triggers"] = trigs
        tpls.append(t)
    assert tk.i == len(tk.t), "trailing tokens"
    return tpls

def write_block(b):
    s = " %d " % len(b)
    for st in b:
        s += "%d %d " % (st[0], len(st) - 1) + "".join("%d " % a for a in st[1:])
    return s

def write(tpls):
    o = ["missionsfile version 1\n", " %d\n" % len(tpls)]
    for t in tpls:
        o.append("%s %s %d  %d\n%s \n" % (t["id"], t["name"], t["flags"], t["type"], t["desc"]))
        o.append("\n%d " % len(t["groups"]))
        for g in t["groups"]:
            o.append("%d %d %d %d %d %d  " % (*g[:5], len(g[5])) + "".join("%d " % x for x in g[5]) + "\n")
        o.append("%d\n" % len(t["triggers"]))
        for f, c, q in t["triggers"]:
            o.append("%s %s %s " % f + write_block(c) + write_block(q) + "\n")
        o.append("\n\n")
    return "".join(o)
