# Development SSH tunnels

[中文](../../docs_cn/island/ssh-tunnel.md)

`scripts/ssh-tunnel.sh` maintains development-only SSH local port forwards to services that are not directly reachable from the workstation. It loads repository-root `.env`, validates the tunnel records, and delegates to `scripts/ssh-tunnel.py`. This helper is not installed or shipped as part of a release.

## Configure

Copy `.env.example` to `.env`, then define the ordered tunnel names in `SSH_TUNNELS`. Each name is case-insensitively expanded to `SSH_TUNNEL_<NAME>` and `SSH_TUNNEL_<NAME>_PASSWORD_ENV`:

```sh
SSH_TUNNELS="lab simulator"

SSH_TUNNEL_LAB="developer@robot.example.com:22 50051 8000"
SSH_TUNNEL_LAB_PASSWORD_ENV=LAB_SSH_PASSWORD
LAB_SSH_PASSWORD='replace-me'

SSH_TUNNEL_SIMULATOR="operator@simulator.example.com:2222 50052"
SSH_TUNNEL_SIMULATOR_PASSWORD_ENV=SIMULATOR_SSH_PASSWORD
SIMULATOR_SSH_PASSWORD='replace-me'
```

A tunnel record starts with `user@host:ssh_port` and ends with one or more TCP ports. For every listed port, the helper binds `127.0.0.1:<port>` locally and forwards it to `127.0.0.1:<port>` on that target. Local ports must be unique across targets. Tunnel names may contain letters, digits, and underscores, but must begin with a letter.

The password setting is indirect: `SSH_TUNNEL_LAB_PASSWORD_ENV=LAB_SSH_PASSWORD` tells the helper which environment variable contains that target's password. `.env` is sourced by Bash, so quote values containing shell-special characters. Never commit `.env`; prefer SSH keys and an SSH agent when the target supports them. On the first connection, OpenSSH records a previously unseen host key automatically; a changed host key is still rejected.

When `SSH_TUNNEL_PROXY` or lowercase `http_proxy` is set, the helper connects to SSH through that HTTP CONNECT proxy. The proxy URL remains in the child environment rather than being expanded into the process arguments.

## Run

Start the worker in the foreground:

```sh
./scripts/ssh-tunnel.sh
```

The worker starts one OpenSSH process per target. Connection attempts are bounded to five seconds, and SSH keepalives terminate an unresponsive session after two missed five-second probes. Each failed connection is retried independently after five seconds. Press `Ctrl-C` to terminate the foreground worker and its active SSH processes.

To detach the worker, run:

```sh
./scripts/ssh-tunnel.sh --backend
```

The command prints the worker PID and writes output to `var/ssh-tunnel.log`. Stop that PID explicitly when the forwards are no longer needed. The helper passes passwords to OpenSSH through `SSH_ASKPASS`; it does not write them to the log.

## Troubleshoot

Configuration errors are reported before any SSH process starts. Confirm that `.env` exists at the repository root, every name in `SSH_TUNNELS` has both derived settings, every referenced password variable is non-empty, and all SSH and forwarded ports are integers from 1 through 65535. OpenSSH host-key checks and authentication policy still apply.
