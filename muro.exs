[
  muro: "0.20.36",
  fuel: 5_000_000,
  modules: [
    {"priv/muro/http/request.muro", "c/generated/request.c", [kind: :c]},
    {"priv/muro/http/request.muro", MuroHttp.Request, "lib/muro_http/request.ex"},
    {"priv/muro/http/request.muro", MuroHttp.RequestTest, "test/request_test.exs",
     [kind: :exunit, impl: MuroHttp.Request, imports: false]},
    {"priv/muro/challenge/decide.muro", "c/generated/decide.c", [kind: :c]},
    {"priv/muro/challenge/decide.muro", MuroHttp.Decide, "lib/muro_http/decide.ex"},
    {"priv/muro/challenge/decide.muro", MuroHttp.DecideTest, "test/decide_test.exs",
     [kind: :exunit, impl: MuroHttp.Decide]}
  ]
]
