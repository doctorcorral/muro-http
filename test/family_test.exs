defmodule MuroHttp.FamilyTest do
  use ExUnit.Case, async: false

  # Cross-connection replies. One key is one connection. The emitted
  # dispatcher is MuroHttp.FamilyFsm.Conns. `--backend c` omits the
  # family, so c/host.c is not this module. mix muro.check walks a
  # fixed script. That walk is not the pin's hold rule, and the hold
  # rule is not this test.

  alias MuroHttp.FamilyFsm.{Conns, ReplyIn, ReplyOut}

  test "mix muro.check accepts reply order, a free accept, and one drained turn" do
    assert {:ok, text, ""} = Muro.CLI.Check.run(["priv/muro/http/family.muro"])
    assert text =~ "Muro v0.22.14\n"
    assert text =~ "family conns: turn per key of Act\n"
    assert text =~ "temporal drained: holds in 3 reachable states, 3 transitions\n"
    assert text =~ "temporal reply-order: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "temporal accept-free: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "temporal reply-opens: holds in 3721 reachable states, 10521 transitions\n"
    assert text =~ "All terms check. Evidence never becomes a run.\n"
  end

  test "an accept on one connection opens while another connection has a live reply" do
    state = Conns.new()
    assert {:open, a_accept, state} = Conns.arrive(state, "a", :accept, 0)
    assert {:held, a_reply, state} = Conns.arrive(state, "a", :reply, 1)
    assert {:open, b_reply, state} = Conns.arrive(state, "b", :reply, 0)
    assert {:open, b_accept, state} = Conns.arrive(state, "b", :accept, 1)

    assert {:error, {:held, ^a_reply}} = Conns.resolved(state, a_reply)
    assert {:ok, [], state} = Conns.resolved(state, b_reply)
    assert {:error, {:held, ^a_reply}} = Conns.resolved(state, a_reply)
    assert {:ok, [{^a_reply, "a", :reply}], state} = Conns.resolved(state, a_accept)
    assert {:ok, [], _state} = Conns.resolved(state, b_accept)
  end

  test "replies of one connection open in arrival order, and the other connection does not wait" do
    state = Conns.new()
    assert {:open, a_accept, state} = Conns.arrive(state, "a", :accept, 0)
    assert {:open, b_accept, state} = Conns.arrive(state, "b", :accept, 0)
    assert {:ok, [], state} = Conns.resolved(state, a_accept)
    assert {:ok, [], state} = Conns.resolved(state, b_accept)
    assert {:open, a0, state} = Conns.arrive(state, "a", :reply, 0)
    assert {:held, a1, state} = Conns.arrive(state, "a", :reply, 1)
    assert {:open, b0, state} = Conns.arrive(state, "b", :reply, 0)

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
               {:arrive, "a", :reply, 0},
               {:arrive, "a", :reply, 1},
               {:arrive, "b", :reply, 0},
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
               {:arrive, "a", :reply, 0},
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
end
