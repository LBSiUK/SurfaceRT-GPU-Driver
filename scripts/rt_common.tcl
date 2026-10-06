# rt_common.tcl - shared settings for the rt_*.exp helpers.
#
# Sourced by rt_ssh.exp, rt_doas.exp and rt_scp.exp; not run directly.
# Everything is configured through environment variables:
#
#   RT_HOST      hostname or address of the Surface RT
#                (default: microsoft-surface-rt.local)
#   RT_USER      login user on the device (default: user)
#   RT_PORT      ssh port (default: 22)
#   RT_PASS      password for both ssh and doas. If unset, you are asked
#                for it once (not echoed) the first time a prompt appears.
#   RT_SSH_OPTS  extra options passed to ssh/scp, split on whitespace,
#                e.g. "-o UserKnownHostsFile=/some/where"
#
# New host keys are accepted on first contact (accept-new); a changed key
# is refused, so after reflashing the device remove its old entry with
# `ssh-keygen -R <host>`.

proc rt_getenv {name default} {
    global env
    if {[info exists env($name)] && $env($name) ne ""} {
        return $env($name)
    }
    return $default
}

set rt_host [rt_getenv RT_HOST microsoft-surface-rt.local]
set rt_user [rt_getenv RT_USER user]
set rt_port [rt_getenv RT_PORT 22]

set rt_ssh_opts [list \
    -o StrictHostKeyChecking=accept-new \
    -o PreferredAuthentications=password \
    -o PubkeyAuthentication=no \
    -o NumberOfPasswordPrompts=1]
foreach opt [regexp -all -inline {\S+} [rt_getenv RT_SSH_OPTS ""]] {
    lappend rt_ssh_opts $opt
}

# Password for ssh and doas: RT_PASS, or asked for once on the terminal.
proc rt_password {} {
    global rt_pass rt_user rt_host
    if {[info exists rt_pass]} {
        return $rt_pass
    }
    set rt_pass [rt_getenv RT_PASS ""]
    if {$rt_pass eq ""} {
        set on_tty [expr {![catch {exec tty -s <@stdin}]}]
        if {$on_tty} { stty -echo }
        send_user "Password for $rt_user@$rt_host: "
        expect_user -timeout -1 -re "(\[^\n\]*)\n"
        if {$on_tty} { stty echo }
        send_user "\n"
        set rt_pass [string trimright $expect_out(1,string) "\r"]
    }
    return $rt_pass
}

# Quote a string for use inside single quotes in a remote sh command.
proc rt_shquote {s} {
    return "'[string map [list ' {'\''}] $s]'"
}

# Wait for the spawned ssh/scp, answering any password prompt (ssh's own
# and doas's), then exit with the remote command's status. There is no
# overall time limit, so long installs are fine; Ctrl-C stops it.
proc rt_finish {} {
    set timeout -1
    expect {
        -re {[Pp]assword[^\n]*: ?$} {
            send -- "[rt_password]\r"
            exp_continue
        }
        eof
    }
    catch wait result
    exit [lindex $result 3]
}
