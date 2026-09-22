#!/usr/bin/env python3
# Mock XDG desktop portal for tests/portal_backends_test.cpp: answers the
# Registry, GlobalShortcuts and RemoteDesktop calls the way GNOME does (minus
# the dialogs) and prints "MOCK ERROR" for protocol mistakes. Run it on a
# private bus only — it claims org.freedesktop.portal.Desktop.
import sys

from gi.repository import Gio, GLib

XML = """
<node>
  <interface name="org.freedesktop.host.portal.Registry">
    <method name="Register">
      <arg type="s" direction="in"/><arg type="a{sv}" direction="in"/>
    </method>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.portal.GlobalShortcuts">
    <method name="CreateSession">
      <arg type="a{sv}" direction="in"/><arg type="o" direction="out"/>
    </method>
    <method name="BindShortcuts">
      <arg type="o" direction="in"/><arg type="a(sa{sv})" direction="in"/>
      <arg type="s" direction="in"/><arg type="a{sv}" direction="in"/>
      <arg type="o" direction="out"/>
    </method>
    <signal name="Activated">
      <arg type="o"/><arg type="s"/><arg type="t"/><arg type="a{sv}"/>
    </signal>
    <signal name="Deactivated">
      <arg type="o"/><arg type="s"/><arg type="t"/><arg type="a{sv}"/>
    </signal>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.portal.RemoteDesktop">
    <method name="CreateSession">
      <arg type="a{sv}" direction="in"/><arg type="o" direction="out"/>
    </method>
    <method name="SelectDevices">
      <arg type="o" direction="in"/><arg type="a{sv}" direction="in"/>
      <arg type="o" direction="out"/>
    </method>
    <method name="Start">
      <arg type="o" direction="in"/><arg type="s" direction="in"/>
      <arg type="a{sv}" direction="in"/><arg type="o" direction="out"/>
    </method>
    <method name="NotifyKeyboardKeycode">
      <arg type="o" direction="in"/><arg type="a{sv}" direction="in"/>
      <arg type="i" direction="in"/><arg type="u" direction="in"/>
    </method>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
"""

APP_ID = "io.github.russkiy78.voiceTyper"
PATH = "/org/freedesktop/portal/desktop"

registered = {}  # sender -> app id
used = set()     # senders that already made a portal call
tokens = 0


def log(msg):
    print(msg, flush=True)


def error(msg):
    log("MOCK ERROR: " + msg)


def escaped(sender):
    return sender[1:].replace(".", "_")


def later(ms, fn):
    GLib.timeout_add(ms, lambda: fn() and False)


def respond(conn, sender, options, response, results):
    path = "/org/freedesktop/portal/desktop/request/%s/%s" % (
        escaped(sender), options["handle_token"])
    later(20, lambda: conn.emit_signal(
        sender, path, "org.freedesktop.portal.Request", "Response",
        GLib.Variant("(ua{sv})", (response, results))))
    return path


def on_call(conn, sender, path, iface, method, params, inv):
    global tokens
    args = params.unpack()
    name = iface.rsplit(".", 1)[1] + "." + method
    log("%s %r" % (name, args))

    if name == "Registry.Register":
        if sender in used:
            error("Register after another portal call on the same connection")
        if args[0] != APP_ID:
            error("unexpected app id %r" % args[0])
        registered[sender] = args[0]
        inv.return_value(None)
        return

    used.add(sender)
    if iface.startswith("org.freedesktop.portal.") and sender not in registered:
        error("%s from a connection without a registered app id" % name)

    if method == "CreateSession":
        options = args[0]
        session = "/org/freedesktop/portal/desktop/session/%s/%s" % (
            escaped(sender), options["session_handle_token"])
        request = respond(conn, sender, options, 0,
                          {"session_handle": GLib.Variant("s", session)})
        inv.return_value(GLib.Variant("(o)", (request,)))
    elif name == "GlobalShortcuts.BindShortcuts":
        session, shortcuts, _parent, options = args
        expected = [("dictation", {"description": "Start or stop dictation",
                                   "preferred_trigger": "CTRL+ALT+v"})]
        if shortcuts != expected:
            error("BindShortcuts got %r" % (shortcuts,))
        granted = [(sid, {"description": GLib.Variant("s", props["description"]),
                          "trigger_description": GLib.Variant("s", "Press <Control><Alt>v")})
                   for sid, props in shortcuts]
        request = respond(conn, sender, options, 0,
                          {"shortcuts": GLib.Variant("a(sa{sv})", granted)})
        inv.return_value(GLib.Variant("(o)", (request,)))
        # The user presses and releases the shortcut.
        for delay, signal in ((300, "Activated"), (400, "Deactivated")):
            later(delay, lambda s=signal: conn.emit_signal(
                sender, PATH, iface, s,
                GLib.Variant("(osta{sv})", (session, "dictation", 1, {}))))
    elif name == "RemoteDesktop.SelectDevices":
        _session, options = args
        if options.get("types") != 1 or options.get("persist_mode") != 2:
            error("SelectDevices options %r" % options)
        request = respond(conn, sender, options, 0, {})
        inv.return_value(GLib.Variant("(o)", (request,)))
    elif name == "RemoteDesktop.Start":
        _session, _parent, options = args
        tokens += 1
        request = respond(conn, sender, options, 0,
                          {"devices": GLib.Variant("u", 1),
                           "restore_token": GLib.Variant("s", "tok%d" % tokens)})
        inv.return_value(GLib.Variant("(o)", (request,)))
    elif name == "RemoteDesktop.NotifyKeyboardKeycode":
        inv.return_value(None)
    else:
        error("unhandled " + name)
        inv.return_error_literal(Gio.dbus_error_quark(), Gio.DBusError.UNKNOWN_METHOD, name)


def on_property(conn, sender, path, iface, prop):
    return GLib.Variant("u", 1)


def main():
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    for info in node.interfaces:
        conn.register_object(PATH, info, on_call, on_property, None)
    loop = GLib.MainLoop()
    # The name goes away when dbus-run-session tears the bus down.
    Gio.bus_own_name_on_connection(
        conn, "org.freedesktop.portal.Desktop", Gio.BusNameOwnerFlags.NONE,
        lambda *_: log("MOCK READY"), lambda *_: loop.quit())
    loop.run()


if __name__ == "__main__":
    sys.exit(main())
