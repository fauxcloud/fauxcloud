"""The repository's GitHub files: issue and discussion forms, labels, and the community workflows.

Every .github YAML file parses; every action is pinned by commit with its version in a comment; the stale policy
touches only `needs-info` issues; the pull request gate runs no code from the pull request and holds the constants
CONTRIBUTING.md promises. The YAML is read by a small loader below: the tool tests use the standard library only,
and the loader refuses what it does not know instead of guessing.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GH = ROOT / ".github"
WORKFLOWS = GH / "workflows"

LABELS = ["bug", "needs-triage", "console-report", "accepted", "needs-bench", "needs-info", "stale"]
DISCUSSIONS = "https://github.com/fauxcloud/fauxcloud/discussions"
STALE_MESSAGE = ("No answer for 30 days. This closes in 7 days unless the missing information arrives; "
                 "a comment keeps it open.")
CLOSE_MESSAGE = ("Closed for lack of information. When you have the details, comment here and a maintainer will reopen it, "
                 "or open a new one.")
GATE_MESSAGE = ("Thanks for the pull request. This project takes code only for an issue the maintainer has labelled "
                "`accepted` (see CONTRIBUTING.md): open an issue first, or use the bug report form, and link it here "
                "with \"Fixes #" + chr(0x2026) + "\" once it is accepted, then comment here and a maintainer will reopen "
                "it, or open a new pull request. Forks are always welcome.")


class YamlError(ValueError):
    pass


class MiniYaml:
    """Block mappings and sequences, plain / quoted / block scalars, one-line flow collections, comments.

    Plain scalars resolve as YAML 1.2 core does (so `on` stays a string, as GitHub reads it). Anchors, aliases,
    tags, tabs in indentation, several documents and duplicate keys are refused.
    """

    def __init__(self, text):
        self.lines = text.replace("\r\n", "\n").split("\n")
        self.i = 0

    @staticmethod
    def indent(line):
        return len(line) - len(line.lstrip(" "))

    def fail(self, why):
        raise YamlError(f"line {self.i + 1}: {why}")

    def skip(self):
        while self.i < len(self.lines):
            s = self.lines[self.i].strip()
            if s and not s.startswith("#"):
                return
            self.i += 1

    def load(self):
        for n, line in enumerate(self.lines):
            if line[:self.indent(line)].count("\t") or line.lstrip(" ").startswith("\t"):
                raise YamlError(f"line {n + 1}: a tab in the indentation")
        self.skip()
        if self.i >= len(self.lines):
            return None
        if self.lines[self.i].strip() in ("---", "..."):
            self.fail("document markers are not supported")
        value = self.node(self.indent(self.lines[self.i]))
        self.skip()
        if self.i < len(self.lines):
            self.fail("content after the end of the top-level block")
        return value

    @staticmethod
    def is_item(text):
        return text.startswith("- ") or text.rstrip() == "-"

    def node(self, ind):
        text = self.lines[self.i][ind:]
        if self.is_item(text):
            return self.sequence(ind)
        if self.split_key(text) is not None:
            return self.mapping(ind)
        return self.scalar(text, ind - 1)

    def sequence(self, ind):
        out = []
        while True:
            self.skip()
            if self.i >= len(self.lines):
                return out
            line = self.lines[self.i]
            cur = self.indent(line)
            if cur > ind:
                self.fail("bad indentation in a sequence")
            if cur < ind or not self.is_item(line[ind:]):
                return out
            body = line[ind + 1:]
            if body.strip() == "" or body.strip().startswith("#"):
                self.i += 1
                self.skip()
                if self.i >= len(self.lines) or self.indent(self.lines[self.i]) <= ind:
                    out.append(None)
                else:
                    out.append(self.node(self.indent(self.lines[self.i])))
                continue
            off = ind + 1 + self.indent(body)
            self.lines[self.i] = " " * off + body.lstrip(" ")  # the item's text, at its own column
            out.append(self.node(off))

    def mapping(self, ind):
        out = {}
        while True:
            self.skip()
            if self.i >= len(self.lines):
                return out
            line = self.lines[self.i]
            cur = self.indent(line)
            if cur < ind or (cur == ind and self.is_item(line[ind:])):
                return out
            if cur > ind:
                self.fail("bad indentation in a mapping")
            kv = self.split_key(line[ind:])
            if kv is None:
                self.fail("expected `key: value`")
            key, rest = kv
            if key in out:
                self.fail(f"duplicate key {key!r}")
            out[key] = self.value(rest, ind)

    @staticmethod
    def find_unquoted(text, pred):
        quote = None
        for k, c in enumerate(text):
            if quote:
                if c == quote:
                    quote = None
            elif c in "'\"" and (k == 0 or text[k - 1] in " [{,:"):
                quote = c
            elif pred(text, k):
                return k
        return -1

    def split_key(self, text):
        if text[:1] in "[{&*!|>%@`":
            return None
        k = self.find_unquoted(text, lambda t, j: t[j] == ":" and (j + 1 == len(t) or t[j + 1] == " "))
        if k <= 0:
            return None
        raw = text[:k].strip()
        if " #" in raw or raw.startswith("#"):
            return None
        key = self.quoted(raw) if raw[:1] in "'\"" else raw
        return key, text[k + 1:]

    def strip_comment(self, text):
        k = self.find_unquoted(text, lambda t, j: t[j] == "#" and (j == 0 or t[j - 1] == " "))
        return (text if k < 0 else text[:k]).strip()

    def value(self, rest, ind):
        body = self.strip_comment(rest)
        if body == "":
            self.i += 1
            self.skip()
            if self.i >= len(self.lines):
                return None
            nxt = self.lines[self.i]
            cur = self.indent(nxt)
            if cur > ind:
                return self.node(cur)
            if cur == ind and self.is_item(nxt[ind:]):
                return self.sequence(ind)
            return None
        if body[0] in "|>":
            return self.block_scalar(body, ind)
        return self.scalar(rest, ind)

    def block_scalar(self, header, ind):
        m = re.fullmatch(r"([|>])(-?)", header)
        if not m:
            self.fail(f"unsupported block scalar header {header!r}")
        style, chomp = m.groups()
        self.i += 1
        raw = []
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if line.strip() and self.indent(line) <= ind:
                break
            raw.append(line)
            self.i += 1
        while raw and not raw[-1].strip():
            raw.pop()
        if not raw:
            return ""
        col = min(self.indent(x) for x in raw if x.strip())
        rows = [x[col:] if x.strip() else "" for x in raw]
        if style == "|":
            text = "\n".join(rows)
        else:
            text = rows[0]
            for prev, r in zip(rows, rows[1:]):
                if r == "":
                    text += "\n"
                elif prev == "":
                    text += r
                elif r.startswith(" ") or prev.startswith(" "):
                    text += "\n" + r
                else:
                    text += " " + r
        return text if chomp == "-" else text + "\n"

    def scalar(self, text, ind):
        text = text.strip()
        if text[:1] in "&*!":
            self.fail("anchors, aliases and tags are not supported")
        if text[:1] in "'\"":
            close = self.closing_quote(text)
            if close < 0:
                self.fail("a quoted scalar must end on its line")
            if self.strip_comment(text[close + 1:]):
                self.fail("text after a quoted scalar")
            self.i += 1
            return self.quoted(text[:close + 1])
        if text[:1] in "[{":
            body = self.strip_comment(text)
            self.i += 1
            return self.flow(body)
        parts = [self.strip_comment(text)]
        self.i += 1
        while self.i < len(self.lines):  # a plain scalar may continue on more-indented lines
            line = self.lines[self.i]
            if not line.strip() or line.strip().startswith("#") or self.indent(line) <= ind:
                break
            if self.split_key(line.strip()) is not None or self.is_item(line.strip()):
                break
            parts.append(self.strip_comment(line))
            self.i += 1
        if any(": " in p or p.endswith(":") for p in parts):  # YAML: "mapping values are not allowed here"
            self.fail("': ' inside a plain scalar (quote it or use a block scalar)")
        return self.resolve(" ".join(parts))

    @staticmethod
    def closing_quote(text):
        q = text[0]
        k = 1
        while k < len(text):
            c = text[k]
            if q == '"' and c == "\\":
                k += 2
                continue
            if c == q:
                if q == "'" and k + 1 < len(text) and text[k + 1] == "'":
                    k += 2
                    continue
                return k
            k += 1
        return -1

    @staticmethod
    def quoted(text):
        q, body = text[0], text[1:-1]
        if q == "'":
            return body.replace("''", "'")
        out, k = [], 0
        simple = {'"': '"', "\\": "\\", "/": "/", "n": "\n", "t": "\t", "r": "\r", "0": "\0", " ": " "}
        while k < len(body):
            c = body[k]
            if c != "\\":
                out.append(c)
                k += 1
                continue
            e = body[k + 1]
            if e in simple:
                out.append(simple[e])
                k += 2
            elif e in "xuU":
                width = {"x": 2, "u": 4, "U": 8}[e]
                out.append(chr(int(body[k + 2:k + 2 + width], 16)))
                k += 2 + width
            else:
                raise YamlError(f"unsupported escape in {text!r}")
        return "".join(out)

    def flow(self, body):
        opener, closer = body[0], {"[": "]", "{": "}"}[body[0]]
        if not body.endswith(closer):
            self.fail("a flow collection must end on its line")
        inner = body[1:-1].strip()
        items, start, quote, depth = [], 0, None, 0
        for k, c in enumerate(inner + ","):
            if quote:
                if c == quote:
                    quote = None
            elif c in "'\"":
                quote = c
            elif c in "[{":
                depth += 1
            elif c in "]}":
                depth -= 1
            elif c == "," and depth == 0:
                items.append(inner[start:k].strip())
                start = k + 1
        if depth or quote:
            self.fail("unbalanced flow collection")
        items = [x for x in items if x]
        scalar = lambda x: self.quoted(x) if x[:1] in "'\"" else self.resolve(x)  # noqa: E731
        if opener == "[":
            if any(x[:1] in "[{" for x in items):
                self.fail("nested flow collections are not supported")
            return [scalar(x) for x in items]
        out = {}
        for x in items:
            kv = self.split_key(x)
            if kv is None:
                self.fail(f"expected `key: value` in {body!r}")
            out[kv[0]] = scalar(kv[1].strip()) if kv[1].strip() else None
        return out

    @staticmethod
    def resolve(text):
        if text in ("", "~", "null", "Null", "NULL"):
            return None
        if text in ("true", "True", "TRUE"):
            return True
        if text in ("false", "False", "FALSE"):
            return False
        if re.fullmatch(r"[-+]?[0-9]+", text):
            return int(text)
        if re.fullmatch(r"[-+]?([0-9]+\.[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?", text):
            return float(text)
        return text


def load(path):
    return MiniYaml(path.read_text(encoding="utf-8")).load()


def gate_script():
    steps = load(WORKFLOWS / "pr-gate.yml")["jobs"]["gate"]["steps"]
    return steps[0]["with"]["script"]


def js_const(script, name):
    m = re.search(rf"^const {name} = (.+);$", script, re.M)
    if not m:
        raise AssertionError(f"no `const {name} = …;` line in the gate's script")
    return m.group(1)


def js_regex(script, name):
    m = re.fullmatch(r"/(.+)/([a-z]*)", js_const(script, name))
    flags = re.I if "i" in m.group(2) else 0
    return re.compile(m.group(1), flags), m.group(2)


def js_string(script, name):
    m = re.fullmatch(r"'([^'\\]*)'", js_const(script, name))
    if not m:
        raise AssertionError(f"{name} is not a plain single-quoted string")
    return m.group(1)


def js_list(script, name):
    return re.findall(r"'([^']*)'", js_const(script, name))


class LoaderTest(unittest.TestCase):
    def test_the_shapes_the_github_files_use(self):
        doc = MiniYaml(
            "# a comment\n"
            "on:\n"
            "  push:\n"
            "    branches: [main, 'rc']  # trailing\n"
            "  workflow_dispatch:\n"
            "permissions: {}\n"
            "jobs:\n"
            "  a:\n"
            "    steps:\n"
            "      - uses: x/y@abc  # v1\n"
            "        with:\n"
            "          n: -1\n"
            "          s: \"a: b # c\"\n"
            "      - run: |\n"
            "          echo one\n"
            "\n"
            "          # kept: inside the block\n"
            "      - name: plain\n"
            "          continued\n"
            "list:\n"
            "- 'it''s'\n"
            "- folded: >-\n"
            "    one\n"
            "    two\n").load()
        self.assertEqual(doc["on"], {"push": {"branches": ["main", "rc"]}, "workflow_dispatch": None})
        self.assertEqual(doc["permissions"], {})
        steps = doc["jobs"]["a"]["steps"]
        self.assertEqual(steps[0], {"uses": "x/y@abc", "with": {"n": -1, "s": "a: b # c"}})
        self.assertEqual(steps[1], {"run": "echo one\n\n# kept: inside the block\n"})
        self.assertEqual(steps[2], {"name": "plain continued"})
        self.assertEqual(doc["list"], ["it's", {"folded": "one two"}])

    def test_it_refuses_what_it_does_not_know(self):
        for text in ("a: 1\na: 2\n", "a: &x 1\n", "a: *x\n", "a: !!str 1\n", "---\na: 1\n", "a:\n\tb: 1\n",
                     "a: \"open\n  b\"\n", "a: [1, [2]]\n", "a: 1\n  b: 2\n", "a: pip --only-binary :all: -r x\n",
                     "a: b:\n", "a: one\n  two: three\n"):
            with self.subTest(text=text), self.assertRaises(YamlError):
                MiniYaml(text).load()


class GithubFilesTest(unittest.TestCase):
    def test_every_github_yaml_file_parses(self):
        files = sorted(GH.rglob("*.yml")) + sorted(GH.rglob("*.yaml"))
        self.assertGreaterEqual(len(files), 10)
        for path in files:
            with self.subTest(path=path.relative_to(ROOT).as_posix()):
                self.assertIsInstance(load(path), (dict, list))

    def test_every_action_is_pinned_by_commit_with_its_version(self):
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                m = re.match(r"^\s*(?:-\s+)?uses:\s*(\S+)(.*)$", line)
                if not m or m.group(1).startswith("./"):
                    continue
                with self.subTest(at=f"{path.name}:{n}"):
                    self.assertRegex(m.group(1), r"^[A-Za-z0-9-]+/[A-Za-z0-9._-]+@[0-9a-f]{40}$")
                    self.assertRegex(m.group(2), r"^\s+# v[0-9]+\.[0-9]+\.[0-9]+")

    def test_issue_config_routes_ideas_and_questions_to_discussions(self):
        cfg = load(GH / "ISSUE_TEMPLATE" / "config.yml")
        self.assertIs(cfg["blank_issues_enabled"], False)
        self.assertEqual(cfg["contact_links"][0], {
            "name": "Ideas and questions", "url": DISCUSSIONS,
            "about": "Feature ideas, questions, show and tell. Open an issue only for a bug or a console report."})

    def test_the_ideas_discussion_form(self):
        form = load(GH / "DISCUSSION_TEMPLATE" / "ideas.yml")
        self.assertNotIn("labels", form)
        body = form["body"]
        self.assertEqual([(b["type"], b["attributes"]["label"]) for b in body], [
            ("textarea", "What would it do and for whom?"),
            ("textarea", "What do you do today instead?"),
            ("checkboxes", "Before you send it")])
        self.assertIs(body[0]["validations"]["required"], True)
        self.assertNotIn("validations", body[1])
        self.assertEqual([o["label"] for o in body[2]["attributes"]["options"]], ["I looked for an existing idea first"])

    def test_labels_file_is_the_source_of_truth(self):
        rows = load(GH / "labels.yml")
        self.assertEqual([r["name"] for r in rows], LABELS)
        for r in rows:
            with self.subTest(label=r["name"]):
                self.assertEqual(sorted(r), ["color", "description", "name"])
                self.assertRegex(str(r["color"]), r"^[0-9a-f]{6}$")
                self.assertLessEqual(len(r["description"]), 100)

    def test_every_label_a_form_or_workflow_names_exists(self):
        named = set()
        for path in sorted((GH / "ISSUE_TEMPLATE").glob("*.yml")):
            if path.name != "config.yml":
                named.update(load(path)["labels"])
        with_ = load(WORKFLOWS / "stale.yml")["jobs"]["stale"]["steps"][0]["with"]
        for key in ("only-issue-labels", "stale-issue-label", "exempt-issue-labels"):
            named.update(x.strip() for x in with_[key].split(","))
        named.add(js_string(gate_script(), "ACCEPTED_LABEL"))
        self.assertTrue({"bug", "needs-triage", "console-report", "needs-info", "stale", "accepted"} <= named, named)
        self.assertEqual(named - set(LABELS), set())


class StaleWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.wf = load(WORKFLOWS / "stale.yml")

    def test_runs_weekly_and_by_hand(self):
        on = self.wf["on"]
        self.assertEqual(on["schedule"], [{"cron": "0 6 * * 1"}])
        self.assertEqual(on["workflow_dispatch"]["inputs"]["debug_only"]["default"], True)
        self.assertEqual(on["workflow_dispatch"]["inputs"]["debug_only"]["type"], "boolean")

    def test_may_only_write_issues(self):
        self.assertEqual(self.wf["permissions"], {})
        self.assertEqual(list(self.wf["jobs"]), ["stale"])
        self.assertEqual(self.wf["jobs"]["stale"]["permissions"], {"issues": "write"})

    def test_touches_only_needs_info_issues_and_never_a_pull_request(self):
        steps = self.wf["jobs"]["stale"]["steps"]
        self.assertEqual(len(steps), 1)
        self.assertTrue(steps[0]["uses"].startswith("actions/stale@"))
        w = steps[0]["with"]
        self.assertEqual(w["only-issue-labels"], "needs-info")
        self.assertEqual(w["days-before-issue-stale"], 30)
        self.assertEqual(w["days-before-issue-close"], 7)
        self.assertEqual(w["days-before-pr-stale"], -1)
        self.assertEqual(w["days-before-pr-close"], -1)
        self.assertEqual(w["stale-issue-label"], "stale")
        self.assertEqual(w["exempt-issue-labels"].split(","), ["accepted", "needs-bench"])
        self.assertIs(w["remove-stale-when-updated"], True)
        self.assertEqual(w["stale-issue-message"], STALE_MESSAGE)
        self.assertEqual(w["close-issue-message"], CLOSE_MESSAGE)
        self.assertEqual(w["debug-only"], "${{ inputs.debug_only == true }}")
        for generic in ("days-before-stale", "days-before-close", "only-labels", "any-of-labels", "stale-pr-message",
                        "close-pr-message", "delete-branch"):
            self.assertNotIn(generic, w)


class PrGateTest(unittest.TestCase):
    def setUp(self):
        self.text = (WORKFLOWS / "pr-gate.yml").read_text(encoding="utf-8")
        self.wf = load(WORKFLOWS / "pr-gate.yml")
        self.script = gate_script()

    def test_trigger_and_permissions(self):
        # synchronize: code pushed after a documentation-only start is checked too
        self.assertEqual(self.wf["on"], {"pull_request_target": {"types": ["opened", "edited", "reopened", "synchronize"]}})
        self.assertEqual(self.wf["permissions"], {})
        self.assertEqual(list(self.wf["jobs"]), ["gate"])
        self.assertEqual(self.wf["jobs"]["gate"]["permissions"],
                         {"pull-requests": "write", "issues": "read", "contents": "none"})

    def test_runs_nothing_from_the_pull_request(self):
        steps = self.wf["jobs"]["gate"]["steps"]
        self.assertEqual(len(steps), 1)
        self.assertTrue(steps[0]["uses"].startswith("actions/github-script@"))
        self.assertEqual(list(steps[0]), ["uses", "with"])
        self.assertEqual(list(steps[0]["with"]), ["script"])  # the default token, with the job's permissions
        self.assertNotIn("checkout", self.text)
        self.assertNotRegex(self.text, r"(?m)^\s*(?:-\s+)?run:")
        self.assertNotIn("${{", self.script)  # event data reaches the script as data, never as code
        self.assertEqual(self.text.count("${{"), 1)
        self.assertIn("group: pr-gate-${{ github.event.pull_request.number }}", self.text)
        logged = re.findall(r"core\.\w+\((.*)\);", self.script)
        self.assertEqual(len(logged), 5)
        for line in logged:  # only numbers reach the log: a line from the pull request could be a workflow command
            for part in re.findall(r"\$\{([^}]*)\}", line):
                self.assertIn(part, ("number", "n", "files.length", "numbers.length"), line)

    def test_side_effects_are_one_comment_and_a_close(self):
        calls = re.findall(r"github\.rest\.(\w+\.\w+)", self.script)
        self.assertEqual(sorted(set(calls)),
                         ["issues.createComment", "issues.get", "issues.listComments", "pulls.listFiles", "pulls.update"])
        self.assertEqual(self.script.count("issues.createComment("), 1)
        self.assertEqual(self.script.count("pulls.update("), 1)
        self.assertIn("state: 'closed'", self.script)
        self.assertNotRegex(self.script, r"state:\s*'open'")
        self.assertIn("body: MESSAGE }", self.script)
        self.assertLess(self.script.index("issues.createComment("), self.script.index("pulls.update("))
        # a missing issue is a missing link; every other error fails the job before the close
        self.assertRegex(self.script, r"if \(e\.status === 404 \|\| e\.status === 410\) continue;\s*throw e;")

    def test_the_message_is_the_constant(self):
        s = self.script
        message = js_const(s, "MESSAGE")
        self.assertEqual(message, "'" + GATE_MESSAGE + "'")
        first = js_string(s, "FIRST_SENTENCE")
        self.assertTrue(GATE_MESSAGE.startswith(first + " "))
        self.assertEqual(first, "Thanks for the pull request.")
        self.assertEqual(js_string(s, "BOT_LOGIN"), "github-actions[bot]")

    def test_exemptions(self):
        s = self.script
        self.assertEqual(js_list(s, "EXEMPT_ASSOCIATIONS"), ["OWNER", "MEMBER"])
        self.assertEqual(js_list(s, "EXEMPT_LOGINS"), ["dependabot[bot]"])
        self.assertEqual(js_string(s, "DOCS_PREFIX"), "docs/")
        self.assertEqual(js_list(s, "DOCS_ROOT_FILES"), ["README.md", "CONTRIBUTING.md"])
        self.assertEqual(js_string(s, "ACCEPTED_LABEL"), "accepted")
        self.assertEqual(js_const(s, "MAX_REFS"), "10")
        self.assertEqual(js_const(s, "MAX_FILES"), "3000")
        self.assertIn("isDocs(f.filename) && (!f.previous_filename || isDocs(f.previous_filename))", s)

    def test_the_link_matchers(self):
        keyword, kflags = js_regex(self.script, "KEYWORD_REF")
        url, uflags = js_regex(self.script, "URL_REF")
        self.assertEqual((kflags, uflags), ("gi", "gi"))
        found = lambda t: [int(m.group(1)) for r in (keyword, url) for m in r.finditer(t)]  # noqa: E731
        for text, numbers in (
                ("Fixes #12", [12]), ("fixes: #3", [3]), ("CLOSES #4 and resolved #5", [4, 5]), ("Fix#6", [6]),
                ("resolve : #7", [7]), ("closed #8.", [8]),
                ("see https://github.com/fauxcloud/fauxcloud/issues/31", [31]),
                ("Fixes https://github.com/fauxcloud/fauxcloud/issues/9#issuecomment-1", [9]),
                ("Fixes #<!-- the accepted issue -->", []),  # the pull request template, untouched
                ("prefix #5", []), ("suffixes #5", []), ("Fixes #12abc", []), ("Fixes 12", []), ("#12", []),
                ("https://github.com/someone/fauxcloud/issues/3", []),
                ("https://github.com/fauxcloud/fauxcloud/pull/3", [])):
            with self.subTest(text=text):
                self.assertEqual(found(text), numbers)


if __name__ == "__main__":
    unittest.main()
