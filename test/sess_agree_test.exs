defmodule MuroHttp.SessAgreeTest do
  use ExUnit.Case, async: false

  # Conformance of the emitted C stepper against MuroHttp.SessionFsm.
  # muro's own check of the emitter is a walk of at most 14 states and
  # depth at most 6. This test is not that walk, and it is not a proof.
  # The step label is the string both steppers print for the action.

  setup_all do
    root = File.cwd!()
    out = Path.join(System.tmp_dir!(), "muro-http-sess-agree")
    File.mkdir_p!(out)
    session_o = Path.join(out, "session.o")
    test_o = Path.join(out, "test_sess.o")
    shim_o = Path.join(out, "shim.o")
    bin = Path.join(out, "test_sess")

    cc!(
      ~w[-Wall -Wextra -Werror -std=c11 -I c -I c/generated -include c/shim.h -c -o] ++
        [session_o, "c/generated/session.c"]
    )

    cc!(~w[-Wall -Wextra -Werror -std=c11 -I c -I c/generated -c -o] ++ [test_o, "c/test_sess.c"])

    cc!(~w[-Wall -Wextra -Werror -std=c11 -I c -c -o] ++ [shim_o, "c/shim.c"])
    cc!(["-o", bin, test_o, session_o, shim_o])
    {:ok, bin: bin, root: root}
  end

  test "two keep-alive requests agree, past the emitter walk depth", %{bin: bin} do
    script = """
    name conn
    init
    send pick 0
    recv pick 0
    recv data piece-req
    send data reply-ok
    send pick 0
    send pick 0
    recv pick 0
    recv data piece-req
    send data reply-ok
    send pick 1
    recv data piece-req
    """

    assert script |> moves() |> length() > 6
    agree!(bin, script)
  end

  test "a chunk, then a request, then close", %{bin: bin} do
    agree!(bin, """
    name conn
    init
    send pick 0
    recv pick 0
    recv data piece-chunk
    send pick 0
    recv pick 0
    recv data piece-req
    send data reply-ok
    send pick 1
    """)
  end

  test "a bad request is one reply and then end", %{bin: bin} do
    agree!(bin, """
    name conn
    init
    send pick 0
    recv pick 0
    recv data piece-bad
    send data reply-bad
    send data reply-ok
    recv data piece-req
    """)
  end

  test "half-close answers once and then refuses another piece", %{bin: bin} do
    agree!(bin, """
    name conn
    init
    send pick 0
    recv pick 1
    recv pick 0
    recv data piece-req
    send data reply-ok
    recv data piece-req
    """)
  end

  test "bare fin and idle timeout end with no reply", %{bin: bin} do
    agree!(bin, """
    name conn
    init
    send pick 0
    recv pick 1
    recv pick 1
    """)

    agree!(bin, """
    name conn
    init
    send pick 1
    """)
  end

  test "an off-protocol event is refused and the next legal move still agrees", %{bin: bin} do
    agree!(bin, """
    name conn
    init
    recv pick 0
    send data reply-ok
    send pick 2
    send nat 0
    send pick 0
    recv pick 0
    recv data piece_req
    send data reply_ok
    send pick 1
    """)
  end

  test "the peer end agrees on one request then close", %{bin: bin} do
    agree!(bin, """
    name peer
    init
    recv pick 0
    send pick 0
    send data piece-req
    recv data reply-ok
    recv pick 1
    send data piece-chunk
    """)
  end

  test "a wrong key, a bad position, and an unknown name are step refusals", %{bin: bin} do
    blocks =
      run!(bin, """
      name conn
      init
      set key 99
      send pick 0
      name conn
      init
      set at 40000
      send pick 0
      name missing
      init
      send pick 0
      """)

    assert length(blocks) == 8

    [init1, key, key_step, init2, at, at_step, missing_init, missing_step] = blocks

    assert init1.status == 0
    assert init1.done == 0
    assert init1.expected == "send_pick"
    assert init1.step =~ "first choose"

    assert key.status == 0
    assert key.done == 0
    assert key.expected == "done"
    assert key.step =~ "session conn: end"

    assert key_step.status == 1
    assert key_step.done == 0
    assert key_step.expected == "done"
    assert key_step.step =~ "session conn: end"

    assert init2.status == 0
    assert at.status == 0
    assert at.done == 0
    assert at.expected == "done"
    assert at.step =~ "session conn: end"

    assert at_step.status == 1
    assert at_step.done == 0
    assert at_step.expected == "done"
    assert at_step.step =~ "session conn: end"

    assert missing_init.status == 3
    assert missing_init.done == 0
    assert missing_init.step == ""
    assert missing_init.expected == "done"

    assert missing_step.status == 1
    assert missing_step.done == 0
    assert missing_step.step == ""
    assert missing_step.expected == "done"

    assert_raise ArgumentError, fn ->
      MuroHttp.SessionFsm.muro_fsm_init("missing", [])
    end
  end

  test "the emitted header has the step statuses and no down step", %{root: root} do
    header = File.read!(Path.join(root, "c/generated/session.h"))
    assert header =~ "MURO_SESS_UNEXPECTED"
    assert header =~ "MURO_SESS_INIT"
    assert header =~ "muro__sess_conn_step"
    assert header =~ "muro__sess_peer_step"
    refute header =~ "muro__sess_down"
    source = File.read!(Path.join(root, "c/generated/session.c"))
    refute source =~ "muro__sess_down"
  end

  defp agree!(bin, script) do
    blocks = run!(bin, script)
    {mod, cmds} = prep(script)
    assert length(cmds) == length(blocks)

    Enum.reduce(Enum.zip(cmds, blocks), nil, fn {cmd, block}, state ->
      case cmd do
        "init" ->
          assert block.status == 0
          next = mod.init()
          assert block.step == Map.fetch!(next, :s)
          assert norm(block.expected) == mod.expected(next)
          assert block.done == bool_int(mod.done?(next))
          next

        _ ->
          {:move, dir, value} = parse_move(cmd)

          case apply(mod, dir, [state, value]) do
            {:ok, next} ->
              assert block.status == 0
              assert block.step == Map.fetch!(state, :s)
              assert norm(block.expected) == mod.expected(next)
              assert block.done == bool_int(mod.done?(next))
              next

            {:error, {:unexpected, step, expected, _value}} ->
              assert block.status == 1
              assert block.step == step
              assert block.step == Map.fetch!(state, :s)
              assert norm(block.expected) == expected
              assert block.done == bool_int(mod.done?(state))
              state
          end
      end
    end)
  end

  defp prep(script) do
    case commands(script) do
      ["name conn" | rest] -> {MuroHttp.SessionFsm.Conn, rest}
      ["name peer" | rest] -> {MuroHttp.SessionFsm.Peer, rest}
    end
  end

  defp moves(script) do
    script
    |> commands()
    |> Enum.filter(fn cmd ->
      match?(["send" | _], String.split(cmd)) or match?(["recv" | _], String.split(cmd))
    end)
  end

  defp commands(script) do
    script
    |> String.split("\n")
    |> Enum.map(&String.trim/1)
    |> Enum.reject(&(&1 == "" or String.starts_with?(&1, "#")))
  end

  defp parse_move(cmd) do
    case String.split(cmd) do
      [dir, "pick", n] when dir in ["send", "recv"] ->
        {:move, String.to_atom(dir), {:pick, pick_bit(n)}}

      [dir, "data", ctor] when dir in ["send", "recv"] ->
        {:move, String.to_atom(dir), String.to_atom(String.replace(ctor, "-", "_"))}

      [dir, "nat", n] when dir in ["send", "recv"] ->
        {:move, String.to_atom(dir), String.to_integer(n)}
    end
  end

  defp pick_bit("0"), do: :b0
  defp pick_bit("1"), do: :b1
  defp pick_bit(n), do: String.to_atom("b" <> n)

  defp norm("send_pick"), do: {:send_pick, [:b0, :b1]}
  defp norm("recv_pick"), do: {:recv_pick, [:b0, :b1]}
  defp norm("done"), do: :done

  defp norm("send (data " <> rest) do
    {:send, {:data, String.trim_trailing(rest, ")"), []}}
  end

  defp norm("recv (data " <> rest) do
    {:recv, {:data, String.trim_trailing(rest, ")"), []}}
  end

  defp bool_int(true), do: 1
  defp bool_int(false), do: 0

  defp run!(bin, script) do
    path =
      Path.join(System.tmp_dir!(), "muro-http-sess-script-#{System.unique_integer([:positive])}")

    File.write!(path, script)

    try do
      {out, status} = System.cmd(bin, [path], stderr_to_stdout: true)
      assert status == 0, out
      parse_blocks(out)
    after
      File.rm(path)
    end
  end

  defp parse_blocks(out) do
    out
    |> String.split("\n", trim: true)
    |> Enum.chunk_by(&(&1 == "."))
    |> Enum.reject(&(&1 == ["."]))
    |> Enum.map(&parse_block/1)
  end

  defp parse_block(lines) do
    Map.new(lines, fn line ->
      case String.split(line, " ", parts: 2) do
        ["status", n] -> {:status, String.to_integer(n)}
        ["done", n] -> {:done, String.to_integer(n)}
        ["step", step] -> {:step, step}
        ["expected", expected] -> {:expected, expected}
      end
    end)
  end

  defp cc!(args) do
    {out, status} = System.cmd("cc", args, cd: File.cwd!(), stderr_to_stdout: true)
    assert status == 0, out
  end
end
