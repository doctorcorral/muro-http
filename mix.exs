defmodule MuroHttp.MixProject do
  use Mix.Project

  def project do
    [
      app: :muro_http,
      version: "0.1.0-dev",
      elixir: "~> 1.15",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      description:
        "A Muro HTTP library. A zero-copy request classifier, one connection step, and a host loop that serves it.",
      package: package()
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp deps do
    [
      {:muro, "~> 0.21.14"}
    ]
  end

  defp package do
    [
      licenses: ["MIT"],
      files: ~w(
        lib
        priv/muro
        c
        mix.exs
        muro.exs
        Makefile
        README.md
        LICENSE
        CHANGELOG.md
        GAPS.md
        .formatter.exs
      )
    ]
  end
end
