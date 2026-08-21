# clippy — asking a model from a zapp

> **Status: not built or run.** This landed in a checkout with no west
> workspace (`zephyr/` and `modules/` are west-managed and gitignored, and
> there is no `.west/`), so none of the firmware here has been compiled, and
> the one open question below has not been settled. The host-side proxy *is*
> verified against the live Space. Read the last section before trusting any
> of this.

Clippy asks a Hugging Face Space a question about Zephyr and shows the answer.
It is the first zapp that waits on something the desktop does not control.

## Why it needed a new piece of ABI

Every call in the ABI through 0.7 finishes before it returns. That was fine
while a zapp only read a file or drew a window. It stops being fine the moment
a zapp wants an answer from a model: the Space runs its model on CPU and takes
tens of seconds, and a zapp runs as a callback inside `lv_timer_handler()` on
the thread that draws the screen. A blocking call would stop the clock, the
taskbar and every other window for the whole request.

So 0.8 adds three calls and one event:

| | |
|---|---|
| `http_request(url, body, &id)` | starts work, returns immediately |
| `ZD_EV_HTTP_RESPONSE` | the answer, later, from the desktop loop |
| `http_read(id, from, buf, cap)` | copy the body out |
| `http_release(id)` | give the slot back |

This is exactly the shape `input/keys.c` already uses, and for the same reason.
A key arrives on Zephyr's input thread; everything downstream of routing it is
LVGL's, and only the desktop thread may touch LVGL. So keys go on a `k_msgq`
and `zd_keys_pump()` drains them from the loop. An HTTP response is the same
problem with a longer fuse, so it takes the same route: a worker thread, a
queue, and `zd_http_pump()` sitting next to `zd_keys_pump()` in `main()`.

CLAUDE.md says a new input modality inherits the queue. A response arriving 40
seconds after the question is a new input modality.

### Two details that are load-bearing

**The body is not in the event.** `struct zd_event` stays a small fixed thing,
and a zapp never holds a pointer into desktop memory across a dispatch it does
not control. The body lives in a desktop-owned slot until `http_release()`.

**Ownership is resolved on the desktop thread, not the worker.** The worker
compares the owner pointer and never dereferences it. If the instance exits
while a request is in flight, `zd_http_owner_gone()` clears the owner and
`zd_http_pump()` drops the response and frees the slot. A response dispatched
into an extension whose text has been unmapped is the failure mode this project
treats as unacceptable rather than untidy.

## Why the TLS hop is the proxy's job

`tools/hf-proxy.py` runs on the development host and does three things the
device should not have to:

1. **Terminates TLS.** On-device HTTPS means mbedTLS, a CA bundle, and its
   expiry. That is a lot of flash and a lot of maintenance to reach one Space.
2. **Performs Gradio's two-step call.** A prediction is a POST that returns an
   event id, then a GET that streams server-sent events until one says
   `complete`.
3. **Answers in plain text.** This is the one that matters most: it means the
   zapp needs no JSON parser, and a zapp has no libc to write one with.

The device therefore speaks the simplest HTTP there is — POST a question, get
an answer.

`http_request()` **rejects `https://` rather than downgrading it.** Silently
sending in the clear what a caller asked to encrypt is the kind of
helpfulness that ends up in an advisory.

## Running it

```bash
python tools/hf-proxy.py                        # on the development host
west build -b qemu_cortex_a53 app -- -DCONFIG_ZD_NET=y
west build -t run
```

The proxy binds `0.0.0.0:8080` so QEMU's user-mode alias `10.0.2.2` can reach
it. That address is compiled into `zapps/clippy/clippy.c`; a real device would
point somewhere reachable on its own network.

Point it at the ROS 2 assistant instead:

```bash
python tools/hf-proxy.py --space eoinedge/ros2
```

The proxy is a development tool. No authentication, no rate limiting; do not
expose it to a network you do not control.

## What is verified and what is not

**Verified.** The proxy, end to end against the live Space:

```
  -> How do I define a devicetree binding?
  <- 2809 bytes
```

It returned real Zephyr documentation with source citations and similarity
scores, flattened out of markdown into something a fixed-width text widget can
show.

**Not verified.** Everything on the device. No build, no run, no `nm -u` check
on `clippy.llext` — and that last one matters, because this project's standing
trap is the compiler emitting a call nobody wrote. Clippy avoids the two known
ones on purpose: `z_zero()` instead of a struct assignment that GCC turns into
`memset`, and a `uint32_t` narrowing before the elapsed-seconds divide so
Xtensa does not reach for `__divdi3`. Those are precautions taken from the
documented history, not observations.

**The open question.** `qemu_cortex_a53` has no ethernet in its devicetree, and
I could not check whether the Zephyr revision this project pins carries a
virtio-net driver that would bind to `virtio-mmio-bus.5`. If it does not, the
transport has to change — Zephyr's usual QEMU networking is SLIP over a spare
UART with `net-tools` on the host, which works on any board but needs more host
setup than user-mode networking. **Nothing above the transport changes either
way**: the ABI, the worker, the pump, the zapp and the proxy are all unaffected.

That is the first thing to settle when a west workspace is available:

```bash
west build -b qemu_cortex_a53 app -- -DCONFIG_ZD_NET=y
```

and read the Kconfig warnings per board, as CLAUDE.md insists — a knob that
does not exist on a target is a knob that silently does nothing.
