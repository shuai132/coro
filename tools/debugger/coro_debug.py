"""Read-only async stacks for coro, loaded by GDB source or LLDB command script import."""

import shlex


REGISTRY = "coro::detail::async_debug_registry::"
MAX_NODES = 4096
MAX_DEPTH = 256


def snapshot(backend):
    """Read only documented metadata, never compiler-specific coroutine frame layouts."""
    if backend.global_int("updating_"):
        raise RuntimeError("async registry update interrupted; retry at a stable stop (core may be incomplete)")
    pointer = backend.head()
    nodes = {}
    seen = set()
    previous = 0
    while backend.integer(pointer):
        address = backend.integer(pointer)
        if address in seen or len(seen) >= MAX_NODES:
            raise RuntimeError("async registry is cyclic or exceeds %d records" % MAX_NODES)
        seen.add(address)
        value = backend.dereference(pointer)
        number = lambda name: backend.integer(backend.field(value, name))
        if number("prev_") != previous:
            raise RuntimeError("inconsistent async registry links; memory may be incomplete")
        identity = number("id_")
        if not identity or identity in nodes or not number("frame_"):
            raise RuntimeError("invalid async registry record; memory may be incomplete")
        nodes[identity] = {
            "id": identity,
            "frame": number("frame_"),
            "parent": number("parent_id_"),
            "external": number("external_parent_"),
            "completed": bool(number("completed_")),
            "function": backend.string(backend.field(value, "function_")),
            "file": backend.string(backend.field(value, "file_")),
            "line": number("line_"),
        }
        previous = address
        pointer = backend.field(value, "next_")
    return nodes


def describe(node):
    return "id:%d frame=0x%x [%s] %s at %s:%d" % (
        node["id"], node["frame"], "completed" if node["completed"] else "live",
        node["function"] or "<unknown coroutine>", node["file"] or "<unknown>", node["line"],
    )


def backtrace(nodes, identity):
    lines = []
    seen = set()
    while identity:
        if identity in seen:
            lines.append("<cycle at id:%d>" % identity)
            break
        if len(seen) >= MAX_DEPTH:
            lines.append("<depth limit reached>")
            break
        node = nodes.get(identity)
        if node is None:
            lines.append("<parent id:%d no longer live or unavailable>" % identity)
            break
        seen.add(identity)
        lines.append("#%d %s" % (len(seen) - 1, describe(node)))
        if node["external"]:
            lines.append("<external parent frame=0x%x; metadata unavailable>" % node["external"])
            break
        identity = node["parent"]
    return "\n".join(lines)


def render(nodes, command, argument):
    args = shlex.split(argument)
    if command == "list":
        if args:
            raise ValueError("usage: coro-list")
        return "\n".join(describe(nodes[key]) for key in sorted(nodes)) or "No live coroutine frames."
    if len(args) != 1:
        raise ValueError("usage: coro-bt <frame-address|id:N> (get addresses and IDs with coro-list)")
    token = args[0]
    if token.startswith("id:"):
        identity = int(token[3:], 10)
    else:
        address = int(token, 0)
        identity = next((key for key, node in nodes.items() if node["frame"] == address), None)
    if identity not in nodes:
        raise ValueError("coroutine not found; use coro-list to select a live frame")
    return backtrace(nodes, identity)


class GdbBackend:
    def __init__(self, gdb):
        self.gdb = gdb
        inferior = gdb.selected_inferior()
        if any(thread.is_running() for thread in inferior.threads()):
            raise RuntimeError("stop all threads before reading async stacks")

    def global_value(self, name):
        return self.gdb.parse_and_eval(REGISTRY + name)

    def global_int(self, name):
        return int(self.global_value(name))

    def head(self):
        return self.global_value("head_")

    @staticmethod
    def integer(value):
        return int(value)

    @staticmethod
    def field(value, name):
        return value[name]

    @staticmethod
    def dereference(value):
        return value.dereference()

    @staticmethod
    def string(value):
        return value.string(encoding="utf-8", errors="replace") if int(value) else ""


class LldbBackend:
    def __init__(self, debugger):
        import lldb
        self.lldb = lldb
        self.target = debugger.GetSelectedTarget()
        self.process = self.target.GetProcess()
        if self.process.GetState() not in (lldb.eStateStopped, lldb.eStateCrashed, lldb.eStateSuspended):
            raise RuntimeError("stop the process or load a core before reading async stacks")

    @staticmethod
    def checked(value):
        if not value.IsValid() or value.GetError().Fail():
            raise RuntimeError("unavailable async metadata: %s" % value.GetError())
        return value

    def global_value(self, name):
        return self.checked(self.target.FindFirstGlobalVariable(REGISTRY + name))

    def global_int(self, name):
        return self.integer(self.global_value(name))

    def head(self):
        return self.global_value("head_")

    def integer(self, value):
        error = self.lldb.SBError()
        result = self.checked(value).GetValueAsUnsigned(error)
        if error.Fail():
            raise RuntimeError(str(error))
        return result

    def field(self, value, name):
        return self.checked(value.GetChildMemberWithName(name))

    def dereference(self, value):
        return self.checked(value.Dereference())

    def string(self, value):
        address = self.integer(value)
        if not address:
            return ""
        error = self.lldb.SBError()
        result = self.process.ReadCStringFromMemory(address, 4096, error)
        if error.Fail():
            # Compact cores can omit the read-only image pages containing source_location strings.
            resolved = self.target.ResolveLoadAddress(address)
            section = resolved.GetSection()
            if not section.IsValid():
                raise RuntimeError(str(error))
            offset = resolved.GetOffset()
            size = min(4096, section.GetByteSize() - offset)
            data = section.GetSectionData(offset, size)
            error.Clear()
            raw = data.ReadRawData(error, 0, size)
            if error.Fail() or not raw or b"\0" not in raw:
                raise RuntimeError("unreadable source string in core and matching image")
            result = raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return result


def error_message(error):
    return ("coro: %s\nRequires CORO_DEBUG_ASYNC_STACK in every translation unit, matching debug symbols, "
            "and readable registry/frame memory. No inferior functions were called." % error)


def lldb_list(debugger, command, result, internal_dict):
    _lldb_command(debugger, "list", command, result)


def lldb_bt(debugger, command, result, internal_dict):
    _lldb_command(debugger, "bt", command, result)


def _lldb_command(debugger, operation, argument, result):
    try:
        result.PutCString(render(snapshot(LldbBackend(debugger)), operation, argument))
    except Exception as error:
        result.SetError(error_message(error))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f %s.lldb_list coro-list" % __name__)
    debugger.HandleCommand("command script add -f %s.lldb_bt coro-bt" % __name__)


def _register_gdb():
    try:
        import gdb
    except ImportError:
        return

    class CoroCommand(gdb.Command):
        def __init__(self, name, operation):
            super().__init__(name, gdb.COMMAND_STACK)
            self.operation = operation

        def invoke(self, argument, from_tty):
            self.dont_repeat()
            try:
                gdb.write(render(snapshot(GdbBackend(gdb)), self.operation, argument) + "\n")
            except Exception as error:
                raise gdb.GdbError(error_message(error))

    CoroCommand("coro-list", "list")
    CoroCommand("coro-bt", "bt")


_register_gdb()
