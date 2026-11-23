defmodule MuroHttp.StepSliceTest do
  use ExUnit.Case, async: false

  # The positive equations live in priv/muro/http/step.muro and are
  # checked by mix muro.gen. This file only refuses the negative book.

  # Loading step.muro rechecks the slice. One run is the whole book.
  @tag timeout: 3_600_000
  test "check refuses a span after the 413 close" do
    path = Path.join(File.cwd!(), "priv/muro/http/neg/step_span.muro")

    assert {:error, message} = Muro.check_file(path, fuel: 5_000_000),
           "span-after-close was accepted"

    refute message =~ "out of fuel"
    assert message =~ "span-after-close"
  end
end
