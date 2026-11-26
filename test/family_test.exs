defmodule MuroHttp.FamilyTest do
  use ExUnit.Case, async: false

  # Per-key reply order. One key is one connection. A reply waits
  # for earlier instances of its own key. Another key does not, and
  # the rule does not order replies across keys. The emitted
  # dispatcher is MuroHttp.FamilyFsm.Conns. `--backend c` omits the
  # family, so c/host.c never calls it. This module is not the poll
  # loop. mix muro.check walks a fixed script. That walk is not the
  # pin's hold rule, and the hold rule is not this test.

  alias MuroHttp.FamilyFsm.{Conns, ReplyIn, ReplyOut}

  # 3721 states and 10521 transitions are a regression pin of the one
  # family graph mix muro.check builds for the fixed script in
  # family.muro: key `a` is accept, then reply, then reply, and key
  # `b` is accept. reply-order, accept-free, and reply-opens share
  # that graph. 10521 is the successor table, not a breadth-first
  # parent map. drained is the one-reply meeting, 3 states and 3
  # transitions. A different count means the script, the meeting, the
  # hold rule, or the checker explored a different graph. The pin is
  # not a bound on live connections and it is not the Agda proof.
  test "mix muro.check accepts reply order, a free accept, and one drained turn" do
    assert {:ok, text, ""} = Muro.CLI.Check.run(["priv/muro/http/family.muro"])
    assert text =~ "Muro v0.22.33\n"
    assert text =~ "family conns: turn per key of Act\n"
    assert text =~ "temporal drained: holds in 3 reachable states, 3 transitions\n"
    assert text =~ "temporal reply-order: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "temporal accept-free: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "temporal reply-opens: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "All terms check. Evidence never becomes a run.\n"
  end

  # Host order on each key: accept, then reply. b's accept is
  # resolved before b's reply, so that reply is not held on b. The
  # family would also open a reply that arrived before that key's
  # accept. The host does not: it accepts the socket before any
  # reply, and it does not read the next request while a response is
  # in progress. This script does not use that order.
  test "an accept on one connection opens while another connection has a live reply" do
    state = Conns.new()
    assert {:open, a_accept, state} = Conns.arrive(state, "a", :accept, 0)
    assert {:held, a_reply, state} = Conns.arrive(state, "a", :reply, 1)
    assert {:open, b_accept, state} = Conns.arrive(state, "b", :accept, 0)
    assert {:ok, [], state} = Conns.resolved(state, b_accept)
    assert {:open, b_reply, state} = Conns.arrive(state, "b", :reply, 1)

    assert {:error, {:held, ^a_reply}} = Conns.resolved(state, a_reply)
    assert {:ok, [], state} = Conns.resolved(state, b_reply)
    assert {:error, {:held, ^a_reply}} = Conns.resolved(state, a_reply)
    assert {:ok, [{^a_reply, "a", :reply}], _state} = Conns.resolved(state, a_accept)
  end

  test "replies of one connection open in arrival order, and the other connection does not wait" do
    state = Conns.new()
    assert {:open, a_accept, state} = Conns.arrive(state, "a", :accept, 0)
    assert {:open, b_accept, state} = Conns.arrive(state, "b", :accept, 0)
    assert {:ok, [], state} = Conns.resolved(state, a_accept)
    assert {:ok, [], state} = Conns.resolved(state, b_accept)
    # 0.22.24 records a resolved index as departed, so a later
    # reply on the same key must use a fresh creation index.
    assert {:open, a0, state} = Conns.arrive(state, "a", :reply, 1)
    assert {:held, a1, state} = Conns.arrive(state, "a", :reply, 2)
    assert {:open, b0, state} = Conns.arrive(state, "b", :reply, 1)

    assert {:error, {:held, ^a1}} = Conns.resolved(state, a1)
    assert {:ok, [], state} = Conns.resolved(state, b0)
    assert {:error, {:held, ^a1}} = Conns.resolved(state, a1)
    assert {:ok, [{^a1, "a", :reply}], _state} = Conns.resolved(state, a0)

    assert :ok =
             Conns.check_trace([
               {:arrive, "a", :accept, 0},
               {:arrive, "b", :accept, 0},
               {:resolved, 0},
               {:resolved, 1},
               {:arrive, "a", :reply, 1},
               {:arrive, "a", :reply, 2},
               {:arrive, "b", :reply, 1},
               {:host, 4, {:recv, :ok}},
               {:peer, 4, {:send, :ok}},
               {:resolved, 4},
               {:host, 2, {:recv, :ok}},
               {:peer, 2, {:send, :ok}},
               {:resolved, 2},
               {:host, 3, {:recv, :ok}},
               {:peer, 3, {:send, :ok}},
               {:resolved, 3}
             ])
  end

  test "a held reply takes no session step, and withdrawing it does not open one" do
    assert {:error, {2, {:held, 1}}} =
             Conns.check_trace([
               {:arrive, "a", :reply, 0},
               {:arrive, "a", :reply, 1},
               {:host, 1, {:recv, :ok}}
             ])

    state = Conns.new()
    assert {:open, a0, state} = Conns.arrive(state, "a", :reply, 0)
    assert {:held, a1, state} = Conns.arrive(state, "a", :reply, 1)
    assert {:ok, [], state} = Conns.withdraw(state, a1)
    assert {:error, {:resolved, ^a1}} = Conns.withdraw(state, a1)
    assert {:ok, [], _state} = Conns.resolved(state, a0)

    assert {:error, {3, {:unknown, 1}}} =
             Conns.check_trace([
               {:arrive, "a", :reply, 0},
               {:arrive, "a", :reply, 1},
               {:withdraw, 1},
               {:host, 1, {:recv, :ok}}
             ])
  end

  test "a reply whose turn has finished stays tracked until it is resolved" do
    assert {:error, {:unresolved, 1, "a", :reply}} =
             Conns.check_trace([
               {:arrive, "a", :accept, 0},
               {:resolved, 0},
               {:arrive, "a", :reply, 1},
               {:host, 1, {:recv, :ok}},
               {:peer, 1, {:send, :ok}}
             ])
  end

  test "the reply turn sends one ok and then refuses another send" do
    state = ReplyOut.init()
    assert {:ok, state} = ReplyOut.send(state, :ok)
    assert ReplyOut.done?(state)

    assert {:error, {:unexpected, _step, :done, :ok}} = ReplyOut.send(state, :ok)

    host = ReplyIn.init()
    assert {:ok, host} = ReplyIn.recv(host, :ok)
    assert ReplyIn.done?(host)
    assert {:error, {:unexpected, _step, :done, :ok}} = ReplyIn.recv(host, :ok)
  end

  # The emitted dispatcher reports a forgotten / departed creation
  # index as {:error, {:departed, key, index}}. There is no
  # :forgotten atom in muro 0.22.24.
  test "a repeat of a departed creation index is refused" do
    state = Conns.new()
    assert {:open, a0, state} = Conns.arrive(state, "a", :accept, 0)
    assert {:ok, [], state} = Conns.resolved(state, a0)
    assert {:error, {:departed, "a", 0}} = Conns.arrive(state, "a", :reply, 0)

    assert {:error, {2, {:departed, "a", 0}}} =
             Conns.check_trace([
               {:arrive, "a", :accept, 0},
               {:resolved, 0},
               {:arrive, "a", :reply, 0}
             ])
  end

  test "skip, skip_below, and drop_key record and clear departed indices" do
    state = Conns.new()
    assert {:ok, state} = Conns.skip(state, "a", 3)
    assert {:error, {:departed, "a", 3}} = Conns.arrive(state, "a", :accept, 3)
    # A later first arrival of an earlier index above the floor is accepted.
    assert {:open, a1, state} = Conns.arrive(state, "a", :accept, 1)
    assert {:error, {:live, "a"}} = Conns.drop_key(state, "a")
    assert {:ok, [], state} = Conns.resolved(state, a1)
    assert {:ok, state} = Conns.drop_key(state, "a")
    # drop_key clears that key's record; the floor stays -1, so 3 may arrive again.
    assert {:open, _a3, _state3} = Conns.arrive(state, "a", :accept, 3)

    state = Conns.new()
    assert {:ok, state} = Conns.skip(state, "b", 0)
    assert {:ok, state} = Conns.skip_below(state, 0)
    assert {:ok, ^state} = Conns.skip_below(state, 0)
    assert {:error, {:departed, "b", 0}} = Conns.arrive(state, "b", :accept, 0)
    assert {:open, _b1, _state} = Conns.arrive(state, "b", :accept, 1)

    assert :ok =
             Conns.check_trace([
               {:skip, "a", 0},
               {:arrive, "a", :accept, 1},
               {:resolved, 0},
               {:drop, "a"},
               {:skip_below, 0}
             ])
  end
end
