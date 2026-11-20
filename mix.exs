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
        "A Muro HTTP/1.1 library. The books and the C host dispatch to a handler. The challenge server is an example.",
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
      {:muro, "~> 0.22.14"}
    ]
  end

  defp package do
    [
      licenses: ["MIT"],
      files: ~w(
        lib
        priv/muro
        c
        examples
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
