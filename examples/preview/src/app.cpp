#include <huxerui/archive.h>
#include <huxerui/huxerui.h>

#include <cstdint>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace huxerui;
using namespace huxerui::archive;

constexpr std::string_view SampleEntry = "example/hello.txt";
constexpr std::string_view SampleText = "Hello from Lib-Archive!\n";
constexpr std::uint64_t PreviewInputLimit = 64ull << 20;

struct PreviewState {
  bool busy = false;
  std::string status;
  std::vector<ArchiveEntry> entries;
};

Task<IoResult<void>> StageArchive(FileReference source, File destination) {
  auto opened = co_await source.OpenReadAsync();
  if (!opened.Succeeded()) co_return IoResult<void>::Failure(std::move(opened).Error());
  auto created = co_await destination.OpenWriteAsync();
  if (!created.Succeeded()) co_return IoResult<void>::Failure(std::move(created).Error());

  auto input = std::move(opened).Value();
  auto output = std::move(created).Value();
  auto remaining = PreviewInputLimit;
  while (true) {
    auto read = co_await input.ReadAsync(64 * 1024);
    if (!read.Succeeded()) co_return IoResult<void>::Failure(std::move(read).Error());
    auto bytes = std::move(read).Value();
    if (bytes.empty()) break;
    if (bytes.size() > remaining) {
      co_return IoResult<void>::Failure({IoErrorCode::TooLarge, "The preview accepts archives up to 64 MiB."});
    }
    remaining -= bytes.size();
    auto written = co_await output.WriteAsync(std::move(bytes));
    if (!written.Succeeded()) co_return IoResult<void>::Failure(std::move(written).Error());
  }
  co_return co_await output.CloseAsync();
}

ArchiveResult<void> CreateSample(const File& destination) {
  auto created = ArchiveWriter::Create(destination, ArchiveFormat::Zip);
  if (!created.Succeeded()) return ArchiveResult<void>::Failure(std::move(created).Error());
  auto writer = std::move(created).Value();
  auto added = writer.AddEntry(SampleEntry, std::as_bytes(std::span(SampleText)));
  if (!added.Succeeded()) return added;
  return writer.Finish();
}

[[huxerui::composable]] View ArchivePreview() {
  const auto state = UseState(PreviewState{});
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto temporary = UseApplication().Directories().temporary_directory;
  const File staged(temporary, "archive-preview-input");
  const File sample(temporary, "archive-preview-sample.zip");
  const File extracted(temporary, "archive-preview-entry");

  return ScrollView {
    Column {
      Text("Archive Preview", TextRole::Title),
      Flow {
        Button("Open archive").With(Enabled(!state->busy && picker->CanOpenFiles())).OnClick([=] {
          state = {.busy = true};
          auto selection = picker->OpenFileAsync({.name = "Archives", .extensions = {"zip", "tar", "gz", "zst"}});
          tasks.Launch([state, staged, selection = std::move(selection)]() mutable -> Task<void> {
            auto selected = co_await std::move(selection);
            if (!selected) {
              state = {.status = "Selection canceled."};
              co_return;
            }
            auto staged_result = co_await StageArchive(std::move(*selected), staged);
            if (!staged_result.Succeeded()) {
              (void)staged.Delete();
              state = {.status = staged_result.Error().message};
              co_return;
            }
            auto opened = ArchiveReader::Open(staged);
            if (!opened.Succeeded()) {
              (void)staged.Delete();
              state = {.status = opened.Error().message};
              co_return;
            }
            auto entries = opened.Value().ListEntries();
            if (!entries.Succeeded()) {
              state = {.status = entries.Error().message};
              co_return;
            }
            state = {.status = "Archive opened.", .entries = std::move(entries).Value()};
          });
        }),
        Button("Save sample ZIP").With(Enabled(!state->busy && picker->CanSaveFiles())).OnClick([=] {
          (void)sample.Delete();
          auto written = CreateSample(sample);
          if (!written.Succeeded()) {
            state.Update([&](PreviewState& value) { value.status = written.Error().message; });
            return;
          }
          state.Update([](PreviewState& value) { value.busy = true; });
          auto saved = picker->SaveFileAsync(sample, {.suggested_name = "example.zip"});
          tasks.Launch([state, saved = std::move(saved)]() mutable -> Task<void> {
            const bool exported = co_await std::move(saved);
            state.Update([&](PreviewState& value) {
              value.busy = false;
              value.status = exported ? "Sample ZIP saved." : "Sample ZIP was not saved.";
            });
          });
        }),
      }.With(Spacing(12.0F)),
      Text(state->status, TextRole::Body),
      ForEach(std::views::iota(std::size_t{0}, state->entries.size()), [=](std::size_t index) {
        const ArchiveEntry entry = state->entries[index];
        return Row {
          Text(entry.path, TextRole::Body),
          Button("Extract").With(Enabled(!state->busy && entry.kind == EntryKind::File && picker->CanSaveFiles()))
              .OnClick([=] {
                (void)extracted.Delete();
                auto opened = ArchiveReader::Open(staged);
                auto result = opened.Succeeded()
                    ? opened.Value().ExtractEntry(entry.path, extracted)
                    : ArchiveResult<void>::Failure(std::move(opened).Error());
                if (!result.Succeeded()) {
                  state.Update([&](PreviewState& value) { value.status = result.Error().message; });
                  return;
                }
                const auto name = entry.path.substr(entry.path.find_last_of('/') + 1);
                state.Update([](PreviewState& value) { value.busy = true; });
                auto saved = picker->SaveFileAsync(extracted, {.suggested_name = name});
                tasks.Launch([state, saved = std::move(saved)]() mutable -> Task<void> {
                  const bool exported = co_await std::move(saved);
                  state.Update([&](PreviewState& value) {
                    value.busy = false;
                    value.status = exported ? "Entry saved." : "Entry was not saved.";
                  });
                });
              }),
        }.With(Spacing(12.0F)).Key(index);
      }),
    }.With(Padding(20.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(ScrollBar());
}

View App() {
  return MaterialTheme { ArchivePreview() };
}

} // namespace

const Application application{
    App,
    {
        .window = {
            .title = "Archive Preview",
            .initial_size = {680.0F, 480.0F},
        },
    }
};
