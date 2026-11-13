# Type-check each book once, emit every manifest target from that
# checked book, and compare the bytes to the committed files.
#
# `mix muro.gen --check` does the same comparison, but it type-checks
# and reloads a book once per manifest entry, and it re-emits a C
# target once for the .c and again for the .h. request.muro is listed
# three times, so that repeated checking was the slow part of CI.

Mix.Tasks.Muro.ensure_started()

defmodule MuroHttp.CheckGenerated do
  @moduledoc false

  def main do
    root = Muro.Manifest.root()

    with {:ok, manifest} <- Muro.Manifest.load(root),
         :ok <- pin(manifest) do
      version = Mix.Tasks.Muro.version()
      {cache, problems} = check_unique_books(manifest, root)

      problems =
        problems ++ compare_targets(manifest, root, version, cache)

      if problems == [] do
        Mix.shell().info("ok")
      else
        Mix.raise(Enum.join(problems, "\n"))
      end
    else
      {:error, message} -> Mix.raise(message)
    end
  end

  defp pin(manifest) do
    Muro.Manifest.version_pin(manifest.version, Mix.Tasks.Muro.version())
  end

  defp check_unique_books(manifest, root) do
    manifest.modules
    |> Enum.map(& &1.book)
    |> Enum.uniq()
    |> Enum.reduce({%{}, []}, fn book_rel, {cache, problems} ->
      entry = Enum.find(manifest.modules, &(&1.book == book_rel))
      path = Muro.Manifest.book_path(root, entry)
      opts = [fuel: manifest.fuel, root: root]
      started = System.monotonic_time()

      case Muro.Load.open(path, opts) do
        {:ok, loaded, lines} ->
          log("checked #{book_rel}", started)
          {Map.put(cache, book_rel, {:ok, loaded, lines}), problems}

        {:error, message} ->
          log("failed #{book_rel}", started)

          exunit? =
            Enum.any?(manifest.modules, &(&1.book == book_rel and Map.get(&1, :kind) == :exunit))

          {open_result, problems} =
            if exunit? do
              open_started = System.monotonic_time()

              case Muro.Load.open(path, Keyword.put(opts, :params, :open)) do
                {:ok, loaded, lines} ->
                  log("checked #{book_rel} (params open)", open_started)
                  {{:ok, loaded, lines}, problems}

                {:error, open_message} ->
                  log("failed #{book_rel} (params open)", open_started)
                  {:error, problems ++ open_errors(manifest, book_rel, open_message, true)}
              end
            else
              {:error, problems}
            end

          cache = Map.put(cache, book_rel, {:error, message, open_result})
          {cache, problems ++ closed_errors(manifest, book_rel, message)}
      end
    end)
  end

  defp closed_errors(manifest, book_rel, message) do
    manifest.modules
    |> Enum.filter(&(&1.book == book_rel and Map.get(&1, :kind) != :exunit))
    |> Enum.map(&does_not_check(&1, message))
  end

  defp open_errors(manifest, book_rel, message, true) do
    manifest.modules
    |> Enum.filter(&(&1.book == book_rel and Map.get(&1, :kind) == :exunit))
    |> Enum.map(&does_not_check(&1, message))
  end

  defp compare_targets(manifest, root, version, cache) do
    Enum.flat_map(manifest.modules, fn entry ->
      case Map.fetch!(cache, entry.book) do
        {:ok, loaded, lines} ->
          compare_entry(entry, root, version, manifest.fuel, loaded, lines, true)

        {:error, _message, {:ok, loaded, lines}} when entry.kind == :exunit ->
          compare_entry(entry, root, version, manifest.fuel, loaded, lines, true)

        {:error, _message, _open} ->
          compare_entry(entry, root, version, manifest.fuel, nil, [], false)
      end
    end)
  end

  defp compare_entry(entry, root, version, fuel, loaded, lines, book_ok?) do
    if book_ok? do
      started = System.monotonic_time()

      case render(root, entry, version, fuel, loaded, lines) do
        {:ok, files} ->
          log("emitted #{entry.target}", started)
          compare_files(files, entry, root)

        {:error, message} ->
          log("failed to emit #{entry.target}", started)

          [
            "#{entry.target}: #{entry.book} is refused by the #{backend_name(entry)} backend\n#{message}"
          ]
      end
    else
      missing_or_stale(entry, root, version)
    end
  end

  defp compare_files(files, entry, root) do
    Enum.flat_map(output_rels(entry), fn rel ->
      path = Path.expand(rel, root)

      cond do
        not File.regular?(path) ->
          ["#{rel}: missing target for #{entry.book}"]

        true ->
          content = File.read!(path)

          case Enum.find(files, &(&1.rel == rel)) do
            %{contents: ^content} ->
              []

            _other ->
              ["#{rel}: differs from what #{entry.book} emits"]
          end
      end
    end)
  end

  defp missing_or_stale(entry, root, version) do
    Enum.flat_map(output_rels(entry), fn rel ->
      path = Path.expand(rel, root)

      if File.regular?(path) do
        classify_without_render(File.read!(path), rel, entry, root, version)
      else
        ["#{rel}: missing target for #{entry.book}"]
      end
    end)
  end

  defp classify_without_render(content, rel, entry, root, version) do
    case Muro.Gen.parse_header(content) do
      :error ->
        ["#{rel}: differs from what #{entry.book} emits"]

      {:ok, header} ->
        current = Muro.Gen.book_hash(root, entry.book)

        version_problem =
          if header.version == version do
            []
          else
            [
              "#{rel}: header says Muro #{header.version}, running #{version}, from #{entry.book}"
            ]
          end

        hash_problem =
          cond do
            header.book != entry.book ->
              ["#{rel}: differs from what #{entry.book} emits"]

            header.book_hash != current ->
              [
                "#{rel}: stale target; #{entry.book} changed (book sha256 #{header.book_hash}, current #{current})"
              ]

            Muro.Gen.body_hash(header.body) != header.body_hash ->
              [
                "#{rel}: hand-edited target; #{entry.book} is unchanged and the body no longer matches its recorded sha256"
              ]

            true ->
              []
          end

        version_problem ++ hash_problem
    end
  end

  defp render(root, entry, version, fuel, loaded, lines) do
    case Map.get(entry, :kind, :elixir) do
      :c ->
        render_native(root, entry, version, loaded, lines, :c)

      :exunit ->
        render_exunit(root, entry, version, fuel, loaded, lines)

      :elixir ->
        render_elixir(root, entry, version, loaded, lines)

      other ->
        {:error, "script/check_generated.exs does not emit kind: #{inspect(other)}"}
    end
  end

  defp render_elixir(root, entry, version, loaded, lines) do
    source =
      loaded
      |> Muro.Prelude.for_emit()
      |> Muro.Grade.erase_book()
      |> then(&Muro.Emit.emit_module(entry.module, &1))

    file = settle(root, entry, version, format_elixir(source, entry.target, root), 3, lines)
    {:ok, [%{rel: entry.target, contents: file}]}
  end

  defp render_exunit(root, entry, version, fuel, loaded, lines) do
    path = Muro.Manifest.book_path(root, entry)

    opts = [
      fuel: fuel,
      root: root,
      book: entry.book,
      imports: Map.get(entry, :imports, false)
    ]

    opts =
      case Map.get(entry, :impl) do
        nil -> opts
        impl -> Keyword.put(opts, :impl, impl)
      end

    case Muro.Emit.ExUnitSuite.render(
           path,
           entry.module,
           Muro.Grade.erase_book(loaded),
           opts
         ) do
      {:ok, %{source: source}} ->
        file = settle(root, entry, version, format_elixir(source, entry.target, root), 3, lines)
        {:ok, [%{rel: entry.target, contents: file}]}

      {:error, _} = err ->
        err
    end
  end

  defp render_native(root, entry, version, loaded, lines, :c) do
    stem = entry.target |> Path.rootname() |> Path.basename()
    book = Muro.Prelude.for_emit(loaded)

    case Muro.Emit.C.prepare(book, stem) do
      {:ok, header, source, _sigs} ->
        book_sha = Muro.Gen.book_hash(root, entry.book)

        files =
          Enum.map([{entry.target, source}, {header_rel(entry.target), header}], fn {rel, body} ->
            body = ensure_newline(body)

            %{
              rel: rel,
              contents:
                Muro.Gen.header(
                  version,
                  entry.book,
                  book_sha,
                  Muro.Gen.body_hash(body),
                  lines,
                  "// "
                ) <> body
            }
          end)

        {:ok, files}

      {:error, _} = err ->
        err
    end
  end

  defp settle(root, entry, version, body, 0, lines) do
    assemble(root, entry, version, body, lines)
  end

  defp settle(root, entry, version, body, left, lines) do
    file = assemble(root, entry, version, body, lines)
    formatted = format_elixir(file, entry.target, root)

    case Muro.Gen.parse_header(formatted) do
      {:ok, %{body: ^body}} -> file
      {:ok, %{body: next}} -> settle(root, entry, version, next, left - 1, lines)
      :error -> file
    end
  end

  defp assemble(root, entry, version, body, lines) do
    Muro.Gen.header(
      version,
      entry.book,
      Muro.Gen.book_hash(root, entry.book),
      Muro.Gen.body_hash(body),
      lines
    ) <> body
  end

  defp format_elixir(source, target, root) do
    formatted =
      if File.regular?(Path.join(root, ".formatter.exs")) do
        {formatter, _opts} =
          Mix.Tasks.Format.formatter_for_file(Path.join(root, target), root: root)

        formatter.(source)
      else
        source |> Code.format_string!() |> IO.iodata_to_binary()
      end

    ensure_newline(IO.iodata_to_binary(formatted))
  end

  defp output_rels(entry) do
    case Map.get(entry, :kind, :elixir) do
      :c -> [entry.target, header_rel(entry.target)]
      _kind -> [entry.target]
    end
  end

  defp header_rel(target), do: Path.rootname(target) <> ".h"

  defp ensure_newline(binary), do: String.trim_trailing(binary, "\n") <> "\n"

  defp does_not_check(entry, message) do
    "#{entry.target}: #{entry.book} does not check\n#{message}"
  end

  defp backend_name(entry) do
    entry
    |> Map.get(:kind, :elixir)
    |> Atom.to_string()
  end

  defp log(label, started) do
    elapsed_ms =
      System.convert_time_unit(System.monotonic_time() - started, :native, :millisecond)

    Mix.shell().info("#{label} in #{format_ms(elapsed_ms)}")
  end

  defp format_ms(ms) when ms < 1000, do: "#{ms}ms"
  defp format_ms(ms), do: :io_lib.format("~.1fs", [ms / 1000]) |> IO.iodata_to_binary()
end

MuroHttp.CheckGenerated.main()
