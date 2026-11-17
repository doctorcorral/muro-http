defmodule MuroHttp.ConnTraceTest do
  use ExUnit.Case, async: false

  # These traces are the same exchanges the HTTP image runs. The image
  # does not name the session. The emitted C stepper is checked against
  # this machine in test/sess_agree_test.exs. That check is conformance.

  alias MuroHttp.SessionFsm.Conn

  @health_keep [
    {:send, {:pick, :b0}},
    {:recv, {:pick, :b0}},
    {:recv, :piece_req},
    {:send, :reply_ok},
    {:send, {:pick, :b0}}
  ]

  @two_then_close @health_keep ++
                    [
                      {:send, {:pick, :b0}},
                      {:recv, {:pick, :b0}},
                      {:recv, :piece_req},
                      {:send, :reply_ok},
                      {:send, {:pick, :b1}}
                    ]

  test "fsm keep-alive then close matches two step requests" do
    assert :ok = Conn.check_trace(@two_then_close, complete: true)

    assert MuroHttp.Conn.sawHealth() == :mode_decide
    assert MuroHttp.Conn.sawIdle() == :mode_idle
    assert MuroHttp.Conn.sawSecond() == :mode_decide
    assert MuroHttp.Conn.sawIdleTick() == :mode_closed
  end

  test "fsm need-more is a chunk, then a request" do
    trace =
      [
        {:send, {:pick, :b0}},
        {:recv, {:pick, :b0}},
        {:recv, :piece_chunk},
        {:send, {:pick, :b0}},
        {:recv, {:pick, :b0}},
        {:recv, :piece_req},
        {:send, :reply_ok},
        {:send, {:pick, :b1}}
      ]

    assert :ok = Conn.check_trace(trace, complete: true)
    assert MuroHttp.Conn.sawNeed() == :mode_read
    assert MuroHttp.Conn.sawHealth() == :mode_decide
  end

  test "fsm malformed sends one reply and ends" do
    trace = [
      {:send, {:pick, :b0}},
      {:recv, {:pick, :b0}},
      {:recv, :piece_bad},
      {:send, :reply_bad}
    ]

    assert :ok = Conn.check_trace(trace, complete: true)
    assert MuroHttp.Conn.sawBad() == :mode_write
    assert MuroHttp.Conn.sawBadDone() == :mode_closed
  end

  test "fsm half-close answers once and then refuses another read" do
    # After the peer picks half-close, it picks again: a piece, or a bare FIN.
    trace = [
      {:send, {:pick, :b0}},
      {:recv, {:pick, :b1}},
      {:recv, {:pick, :b0}},
      {:recv, :piece_req},
      {:send, :reply_ok}
    ]

    assert :ok = Conn.check_trace(trace, complete: true)

    assert {:error, {5, {:unexpected, _step, :done, :piece_req}}} =
             Conn.check_trace(trace ++ [{:recv, :piece_req}])

    assert MuroHttp.Conn.sawHalf() == :mode_half
    assert MuroHttp.Conn.sawHalfWrite() == :mode_half
    assert MuroHttp.Conn.sawHalfDone() == :mode_closed
    assert MuroHttp.Conn.sawFinReq() == :mode_half
  end

  test "fsm bare fin and idle timeout end with no reply" do
    fin = [
      {:send, {:pick, :b0}},
      {:recv, {:pick, :b1}},
      {:recv, {:pick, :b1}}
    ]

    assert :ok = Conn.check_trace(fin, complete: true)
    assert :ok = Conn.check_trace([{:send, {:pick, :b1}}], complete: true)
    assert MuroHttp.Conn.sawEofEmpty() == :mode_closed
    assert MuroHttp.Conn.sawTickRead() == :mode_closed
    assert MuroHttp.Conn.sawIdleTick() == :mode_closed
  end

  test "fsm refuses a write after end and a read after end" do
    done = [
      {:send, {:pick, :b0}},
      {:recv, {:pick, :b0}},
      {:recv, :piece_bad},
      {:send, :reply_bad}
    ]

    assert {:error, {4, {:unexpected, _step, :done, :reply_ok}}} =
             Conn.check_trace(done ++ [{:send, :reply_ok}])

    assert {:error, {4, {:unexpected, _step, :done, :piece_req}}} =
             Conn.check_trace(done ++ [{:recv, :piece_req}])

    closed = MuroHttp.Conn.shut()
    assert MuroHttp.Conn.step(closed, MuroHttp.Conn.evTick()) == closed
    assert MuroHttp.Conn.step(closed, MuroHttp.Conn.evDone()) == closed
    assert MuroHttp.Conn.sawAfterDo() == :do_close
    assert MuroHttp.Conn.sawAfterMode() == :mode_closed
  end

  test "connection close and http/1.0 end instead of looping" do
    trace = [
      {:send, {:pick, :b0}},
      {:recv, {:pick, :b0}},
      {:recv, :piece_req},
      {:send, :reply_ok},
      {:send, {:pick, :b1}}
    ]

    assert :ok = Conn.check_trace(trace, complete: true)
    assert MuroHttp.Conn.sawClose() == :mode_closed
    assert MuroHttp.Conn.sawHttp10() == :mode_closed
  end

  test "a pipelined buffer reports leftover and the tail is a new request" do
    assert MuroHttp.Conn.sawPipe() == :flag_yes
    assert MuroHttp.Conn.sawPipeMode() == :mode_decide
    assert MuroHttp.Conn.sawHealthOne() == :flag_no
  end

  test "check refuses write after close, read after close, and read after half-close" do
    root = File.cwd!()

    for {file, conform} <- [
          {"priv/muro/http/neg/write_closed.muro", "bad-write"},
          {"priv/muro/http/neg/read_closed.muro", "bad-read"},
          {"priv/muro/http/neg/read_half.muro", "bad-half"}
        ] do
      path = Path.join(root, file)

      assert {:error, message} = Muro.check_file(path, fuel: 5_000_000),
             "#{file} was accepted"

      assert message =~ conform
      assert message =~ "does not conform to end"
    end
  end
end
