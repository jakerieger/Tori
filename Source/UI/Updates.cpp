// Copyright 2026 Jake Rieger
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "UI/Updates.hpp"
#include "Core/Utf8.hpp"
#include "Net/Http.hpp"

#include <array>
#include <charconv>
#include <memory>

#include <shellapi.h>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        constexpr std::string_view kRepoOwner = "jakerieger";
        constexpr std::string_view kRepoName  = "Tori";

        // Accepts "1.2.3", "v1.2.3", "1.2", "1.2.3.4", "1.2.3-beta" (suffix ignored).
        using Version = std::array<uint32_t, 4>;  // std::array has a defaulted <=>

        std::optional<Version> ParseVersion(std::string_view S) {
            while (!S.empty() && (S.front() == 'v' || S.front() == 'V'))
                S.remove_prefix(1);

            Version V {};
            std::size_t Idx = 0;
            while (Idx < V.size()) {
                uint32_t N      = 0;
                auto [Ptr, Err] = std::from_chars(S.data(), S.data() + S.size(), N);
                if (Err != std::errc {}) return Idx == 0 ? std::nullopt : std::optional {V};
                V[Idx++] = N;
                S.remove_prefix(static_cast<std::size_t>(Ptr - S.data()));
                if (S.empty() || S.front() != '.') break;
                S.remove_prefix(1);
            }

            return Idx == 0 ? std::nullopt : std::optional {V};
        }

        constexpr std::size_t kMaxResponseSize = 1024 * 1024;  // A release's JSON is a few KiB

        std::optional<std::string> JsonStringField(std::string_view Json, const std::string_view Key) {
            const std::string Needle = "\"" + std::string {Key} + "\"";
            auto Pos                 = Json.find(Needle);
            if (Pos == std::string_view::npos) return std::nullopt;
            Pos = Json.find(':', Pos + Needle.size());

            if (Pos == std::string_view::npos) return std::nullopt;
            Pos = Json.find('"', Pos + 1);

            if (Pos == std::string_view::npos) return std::nullopt;
            const auto End = Json.find('"', Pos + 1);

            if (End == std::string_view::npos) return std::nullopt;

            return std::string {Json.substr(Pos + 1, End - Pos - 1)};
        }

        bool OpenUrl(const std::string& Url) {
            HINSTANCE Result =
              ShellExecuteW(nullptr, L"open", Utf8ToWide(Url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return reinterpret_cast<INT_PTR>(Result) > 32;
        }

        void ShowResult(ToriState& State, const UpdateInfo& Result) {
            State.CheckingForUpdates = false;
            if (!State.Overlay || State.ActiveDialog.IsShown()) { return; }

            std::string Title   = "Error";
            std::string Message = Result.Notes;
            std::string DownloadUrl {};

            switch (Result.Status) {
                case UpdateStatus::UpdateAvailable: {
                    Title       = "Update Available";
                    DownloadUrl = Result.DownloadUrl;
                } break;
                case UpdateStatus::UpToDate: {
                    Title = "Up to Date";
                } break;
                case UpdateStatus::DevelopmentBuild: {
                    Title = "Development Build";
                } break;
                case UpdateStatus::Error: {
                    Message = Result.Notes;
                } break;
            }

            const auto Close          = [Target = &State] { Target->ActiveDialog.Reset(); };
            const auto DownloadLatest = [Target = &State, Url = DownloadUrl] {
                OpenUrl(Url);
                Target->ActiveDialog.Reset();
            };

            std::vector<DialogAction> Actions {};
            if (Result.Status == UpdateStatus::UpdateAvailable) {
                Actions.push_back(DialogAction {.Label     = "Download Latest Version",
                                                .OnPressed = DownloadLatest,
                                                .Variant   = ButtonVariant::Primary});
                Actions.push_back(
                  DialogAction {.Label = "Close", .OnPressed = Close, .Variant = ButtonVariant::Secondary});
            } else {
                Actions.push_back(
                  DialogAction {.Label = "Close", .OnPressed = Close, .Variant = ButtonVariant::Primary});
            }

            State.ActiveDialog = ShowDialog(*State.Overlay,
                                            State.Popup,
                                            Dialog {
                                              .Title     = std::move(Title),
                                              .Content   = Text {.Content = std::move(Message)},
                                              .Actions   = std::move(Actions),
                                              .OnDismiss = Close,
                                            });
        }
    }  // namespace

    UpdateInfo FetchLatestUpdate(const std::stop_token& Stop) {
        UpdateInfo Result {
          .Status = UpdateStatus::Error,
          .Notes  = "Unknown error.",
        };

        const auto Current = ParseVersion(kToriVersion);
        if (!Current) {
            Result.Notes = "Failed to parse current version";
            return Result;
        }

        const std::wstring Path =
          L"/repos/" + Utf8ToWide(kRepoOwner) + L"/" + Utf8ToWide(kRepoName) + L"/releases/latest";
        DWORD Error = 0;
        const auto Response =
          Http::HttpsGet(Stop, L"api.github.com", Path, L"ToriUpdateChecker/1.0", kMaxResponseSize, Error);
        if (!Response) {
            Result.Notes = "Network error while contacting GitHub (WinHTTP error " + std::to_string(Error) + ").";
            return Result;
        }

        if (Response->Status == 404) {
            Result.Notes = "No releases found (404).";
            return Result;
        }

        if (Response->Status != 200) {
            Result.Notes = "GitHub returned HTTP " + std::to_string(Response->Status) +
                           (Response->Status == 403 ? " (possibly rate limited)." : ".");
            return Result;
        }

        const auto Tag = JsonStringField(Response->Body, "tag_name");
        if (!Tag) {
            Result.Notes = "Could not find tag_name in GitHub response";
            return Result;
        }

        Result.Version = *Tag;

        const auto Latest = ParseVersion(*Tag);
        if (!Latest) {
            Result.Notes = "Could not parse latest release tag: " + *Tag;
            return Result;
        }

        const auto Cmp = *Current <=> *Latest;
        if (Cmp < 0) {
            Result.Status = UpdateStatus::UpdateAvailable;
            Result.Notes  = "A newer version of Tori is available: " + *Tag + " (you are running " +
                           std::string {kToriVersion} + ").";
            // The page for exactly the release that was found, rather than whatever is latest by the time it's opened
            Result.DownloadUrl = "https://github.com/" + std::string {kRepoOwner} + "/" + std::string {kRepoName} +
                                 "/releases/tag/" + *Tag;
        } else if (Cmp == 0) {
            Result.Status = UpdateStatus::UpToDate;
            Result.Notes =
              "No update available. You are running the latest version (" + std::string {kToriVersion} + ").";
        } else {
            Result.Status = UpdateStatus::DevelopmentBuild;
            Result.Notes  = "Running development build (" + std::string {kToriVersion} +
                           " is newer than latest release " + *Tag + ").";
        }

        return Result;
    }

    void CheckForUpdates(ToriState& State) {
        if (State.CheckingForUpdates) { return; }
        State.CheckingForUpdates = true;

        // Joined when the next check starts or when the app exits; a result posted after Run returns is never shown
        State.UpdateCheck = std::jthread([Target = &State](const std::stop_token& Stop) {
            UpdateInfo Result = FetchLatestUpdate(Stop);
            if (Stop.stop_requested()) { return; }
            App::Post([Target, Result = std::move(Result)] { ShowResult(*Target, Result); });
        });
    }
}  // namespace Tori::UI
