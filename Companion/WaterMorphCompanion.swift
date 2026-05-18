// WaterMorphCompanion.swift
// Morph — Water library browser and DAW drag companion.
// Native macOS app: no dock icon, floating panel, menu bar toggle.
// Drag originates from THIS window (no sandbox), so NSDraggingSession works.

import Cocoa
import AVFoundation
import UniformTypeIdentifiers
import WebKit
import Darwin

// MARK: - Constants

private let kBaseURL    = "https://water.95ent.ai"
private let kSbURL      = "https://hlqwvctfmxljjmosfcqq.supabase.co"
private let kSbAnonKey  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhscXd2Y3RmbXhsamptb3NmY3FxIiwicm9sZSI6ImFub24iLCJpYXQiOjE3MjY1OTExNDksImV4cCI6MjA0MjE2NzE0OX0.3u7eC9w9mOVvs_yzf1l5aNpamA681EPwVk-94mqseKU"
private let kTCPPort: in_port_t = 59812
private let kTeal       = NSColor(red: 0.102, green: 0.565, blue: 0.627, alpha: 1)
private let kMauve      = NSColor(red: 0.545, green: 0.361, blue: 0.965, alpha: 1)  // #8B5CF6
private let kMauveDark  = NSColor(red: 0.427, green: 0.157, blue: 0.851, alpha: 1)  // #6D28D9
private let kBG         = NSColor(white: 0.039, alpha: 1)   // #0a0a0a — matches web app background
private let kRowBG      = NSColor(white: 0.06,  alpha: 1)

// MARK: - DAW sync state (updated via socket from plugin)

final class WMSyncState {
    static let shared = WMSyncState()
    private(set) var bpm:      Double = 0
    private(set) var key:      String = ""
    private(set) var mode:     String = "project"
    private(set) var timeSecs: Double = 0.0
    private var lastSyncTime: Date = .distantPast
    var onChange: (() -> Void)?

    func update(bpm: Double, key: String, mode: String, timeSecs: Double = 0.0) {
        self.bpm          = bpm
        self.key          = key
        self.mode         = mode
        self.timeSecs     = timeSecs
        self.lastSyncTime = Date()
        onChange?()
    }

    // Connected = received a SYNC in the last 3 seconds
    var isConnected: Bool { Date().timeIntervalSince(lastSyncTime) < 3.0 }
    var bpmLabel:  String { bpm > 0  ? "\(Int(bpm)) BPM" : "— BPM" }
    var keyLabel:  String { key.isEmpty || key == "?" ? "—" : key }
    var modeLabel: String { mode.uppercased() }
}

// MARK: - Data model

struct WMTrack {
    let id: String
    let name: String
    let key: String
    let tags: String
    let bpm: Double
    let duration: Int
    let demoHash: String

    var audioURL: URL {
        // File is always named after the track ID. demo_hash is a cache-buster only.
        let base = "\(kSbURL)/storage/v1/object/public/demo/\(id).mp3"
        if !demoHash.isEmpty {
            return URL(string: "\(base)?v=\(demoHash)")!
        }
        // Fallback: Next.js proxy (public, no auth needed — uses admin client server-side).
        return URL(string: "\(kBaseURL)/api/melody/demo/\(id)")!
    }

    var cachedFile: URL {
        URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("morph-\(id).mp3")
    }

    var displayBPM: String  { bpm > 0 ? "\(Int(bpm))" : "—" }
    var displayKey: String  { key.isEmpty ? "" : key }
    var displayTime: String {
        guard duration > 0 else { return "" }
        return "\(duration / 60):\(String(format: "%02d", duration % 60))"
    }
}

// MARK: - Token store (reads plugin's JUCE settings as primary source)

final class WMTokenStore {
    static let shared = WMTokenStore()
    var accessToken  = ""
    var refreshToken = ""
    var isLoggedIn: Bool { !accessToken.isEmpty }

    private let ownFile: URL = {
        let sup = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return sup.appendingPathComponent("Water/MorphCompanion/token.json")
    }()
    private let pluginFile: URL = {
        let sup = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return sup.appendingPathComponent("Water/Morph/MorphPlugin.settings")
    }()

    func load() {
        // JUCE XML: <VALUE name="authToken" val="eyJ…"/>
        if let xml = try? String(contentsOf: pluginFile, encoding: .utf8),
           let tok = juceValue(xml, name: "authToken"), !tok.isEmpty {
            accessToken  = tok
            refreshToken = juceValue(xml, name: "refreshToken") ?? ""
            return
        }
        if let data = try? Data(contentsOf: ownFile),
           let j = try? JSONSerialization.jsonObject(with: data) as? [String: String] {
            accessToken  = j["access_token"]  ?? ""
            refreshToken = j["refresh_token"] ?? ""
        }
    }

    func save() {
        try? FileManager.default.createDirectory(at: ownFile.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        let d: [String: String] = ["access_token": accessToken, "refresh_token": refreshToken]
        if let data = try? JSONSerialization.data(withJSONObject: d) { try? data.write(to: ownFile) }
        // Keep plugin JUCE settings in sync
        if FileManager.default.fileExists(atPath: pluginFile.path),
           var xml = try? String(contentsOf: pluginFile, encoding: .utf8) {
            xml = setJuceValue(in: xml, name: "authToken",    value: accessToken)
            xml = setJuceValue(in: xml, name: "refreshToken", value: refreshToken)
            try? xml.write(to: pluginFile, atomically: true, encoding: .utf8)
        }
    }

    private func juceValue(_ xml: String, name: String) -> String? {
        guard let r = xml.range(of: "name=\"\(name)\""),
              let vr = xml[r.upperBound...].range(of: "val=\"") else { return nil }
        let s = xml[vr.upperBound...]
        guard let e = s.firstIndex(of: "\"") else { return nil }
        return String(s[..<e])
    }

    private func setJuceValue(in xml: String, name: String, value: String) -> String {
        guard let nameRange = xml.range(of: "name=\"\(name)\""),
              let valStart  = xml[nameRange.upperBound...].range(of: "val=\"") else { return xml }
        let afterQuote = xml[valStart.upperBound...]
        guard let closeQuote = afterQuote.firstIndex(of: "\"") else { return xml }
        return String(xml[..<valStart.upperBound]) + value + String(xml[closeQuote...])
    }
}

// MARK: - API client

final class WMAPIClient {
    static let shared = WMAPIClient()
    private let session = URLSession.shared

    func fetchAllTracks(completion: @escaping (Bool, [WMTrack]) -> Void) {
        fetchPage(1, acc: [], didRefresh: false, completion: completion)
    }

    func refreshTokens(completion: @escaping (Bool) -> Void) {
        let rt = WMTokenStore.shared.refreshToken
        guard !rt.isEmpty, let url = URL(string: "\(kSbURL)/auth/v1/token?grant_type=refresh_token") else {
            completion(false); return
        }
        var req = URLRequest(url: url, timeoutInterval: 15)
        req.httpMethod = "POST"
        req.setValue(kSbAnonKey, forHTTPHeaderField: "apikey")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: ["refresh_token": rt])
        session.dataTask(with: req) { data, resp, _ in
            guard let data, (resp as? HTTPURLResponse)?.statusCode == 200,
                  let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let access = j["access_token"] as? String else {
                DispatchQueue.main.async { completion(false) }; return
            }
            DispatchQueue.main.async {
                WMTokenStore.shared.accessToken  = access
                WMTokenStore.shared.refreshToken = j["refresh_token"] as? String ?? rt
                WMTokenStore.shared.save()
                completion(true)
            }
        }.resume()
    }

    private func fetchPage(_ page: Int, acc: [WMTrack], didRefresh: Bool, completion: @escaping (Bool, [WMTrack]) -> Void) {
        let urlStr = "\(kBaseURL)/api/plugin/v1/tracks?per_page=100&page=\(page)"
        guard let url = URL(string: urlStr) else { DispatchQueue.main.async { completion(false, acc) }; return }
        var req = URLRequest(url: url, timeoutInterval: 15)
        req.setValue("Bearer \(WMTokenStore.shared.accessToken)", forHTTPHeaderField: "Authorization")

        session.dataTask(with: req) { [weak self] data, resp, _ in
            guard let self else { return }
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if status == 401 && !didRefresh {
                self.refreshTokens { ok in
                    if ok { self.fetchPage(page, acc: acc, didRefresh: true, completion: completion) }
                    else  { DispatchQueue.main.async { completion(false, acc) } }
                }
                return
            }
            guard status == 200, let data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let arr  = json["tracks"] as? [[String: Any]] else {
                DispatchQueue.main.async { completion(!acc.isEmpty, acc) }; return
            }
            var tracks = acc
            for t in arr {
                guard let id = t["id"] as? String, let name = t["name"] as? String else { continue }
                tracks.append(WMTrack(
                    id:       id,
                    name:     name,
                    key:      t["key"]       as? String ?? "",
                    tags:     t["tags"]      as? String ?? "",
                    bpm:      t["bpm"]       as? Double ?? 0,
                    duration: t["duration"]  as? Int    ?? 0,
                    demoHash: t["demo_hash"] as? String ?? ""
                ))
            }
            let total = json["total"] as? Int ?? 0
            let more  = (total > 0 && tracks.count < total) || (total == 0 && arr.count == 100)
            if more { self.fetchPage(page + 1, acc: tracks, didRefresh: didRefresh, completion: completion) }
            else    { DispatchQueue.main.async { completion(true, tracks) } }
        }.resume()
    }

    func downloadForDrag(_ track: WMTrack, completion: @escaping (URL?) -> Void) {
        let dest = track.cachedFile
        if FileManager.default.fileExists(atPath: dest.path) { completion(dest); return }
        let req = URLRequest(url: track.audioURL, timeoutInterval: 60)
        // Audio endpoints are public (Supabase CDN or Next.js proxy via admin client).
        // No Bearer header needed.
        session.downloadTask(with: req) { tmp, resp, _ in
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard (200...299).contains(status), let tmp else {
                DispatchQueue.main.async { completion(nil) }; return
            }
            try? FileManager.default.moveItem(at: tmp, to: dest)
            let ok = FileManager.default.fileExists(atPath: dest.path)
            DispatchQueue.main.async { completion(ok ? dest : nil) }
        }.resume()
    }

    func authStart(completion: @escaping (String?) -> Void) {
        guard let url = URL(string: "\(kBaseURL)/api/plugin/v1/auth/start") else { completion(nil); return }
        session.dataTask(with: URLRequest(url: url)) { data, _, _ in
            let json = (try? JSONSerialization.jsonObject(with: data ?? Data())) as? [String: Any]
            let sid = json?["session_id"] as? String
            DispatchQueue.main.async { completion(sid) }
        }.resume()
    }

    func authPoll(_ sid: String, completion: @escaping (String?, String?) -> Void) {
        guard let url = URL(string: "\(kBaseURL)/api/plugin/v1/auth/poll?session_id=\(sid)") else {
            completion(nil, nil); return
        }
        session.dataTask(with: URLRequest(url: url)) { data, resp, _ in
            guard (resp as? HTTPURLResponse)?.statusCode == 200,
                  let data, let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                DispatchQueue.main.async { completion(nil, nil) }; return
            }
            DispatchQueue.main.async { completion(j["access_token"] as? String, j["refresh_token"] as? String) }
        }.resume()
    }
}

// MARK: - Audio player

final class WMAudioPlayer: NSObject, AVAudioPlayerDelegate {
    static let shared = WMAudioPlayer()

    private var localPlayer:  AVAudioPlayer?   // for cached files — most reliable
    private var streamPlayer: AVPlayer?         // fallback for uncached streaming
    private var loadingId = ""                  // track being downloaded for playback

    var currentId = ""
    var onStateChange: (() -> Void)?

    var isPlaying: Bool {
        localPlayer?.isPlaying == true || streamPlayer?.timeControlStatus == .playing
    }

    func toggle(_ track: WMTrack) {
        if currentId == track.id {
            if isPlaying { pause() } else { resume() }
        } else {
            play(track)
        }
    }

    private func play(_ track: WMTrack) {
        stop()
        currentId = track.id
        onStateChange?()

        if FileManager.default.fileExists(atPath: track.cachedFile.path) {
            playLocal(track.cachedFile)
        } else {
            // Download first, then play. Also kicks off file for drag.
            loadingId = track.id
            onStateChange?()
            WMAPIClient.shared.downloadForDrag(track) { [weak self] url in
                guard let self, self.currentId == track.id else { return }
                self.loadingId = ""
                if let url { self.playLocal(url) }
                else       { self.stop() }
            }
        }
    }

    private var progressTimer: Timer?
    var onProgressChange: ((Double, Double) -> Void)?  // (progress 0-1, currentTime secs)

    private func playLocal(_ url: URL) {
        guard let p = try? AVAudioPlayer(contentsOf: url) else { stop(); return }
        localPlayer           = p
        localPlayer?.volume   = 1.0
        localPlayer?.delegate = self
        localPlayer?.prepareToPlay()
        localPlayer?.play()
        startProgressTimer()
        onStateChange?()
    }

    private func startProgressTimer() {
        progressTimer?.invalidate()
        progressTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self, let p = self.localPlayer else { return }
            let prog = p.duration > 0 ? p.currentTime / p.duration : 0
            self.onProgressChange?(prog, p.currentTime)
        }
    }

    private func stopProgressTimer() { progressTimer?.invalidate(); progressTimer = nil }

    var currentDuration: Double { localPlayer?.duration ?? 0 }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully _: Bool) { stop() }
    func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?)     { stop() }

    private func pause()  { localPlayer?.pause(); streamPlayer?.pause(); onStateChange?() }
    private func resume() { localPlayer?.play();  streamPlayer?.play();  onStateChange?() }

    func stop() {
        stopProgressTimer()
        localPlayer?.stop();   localPlayer  = nil
        streamPlayer?.pause(); streamPlayer = nil
        currentId = ""; loadingId = ""
        onStateChange?()
        onProgressChange?(0, 0)
    }

    var isLoadingId: String { loadingId }
}

// MARK: - Waveform view

final class WMWaveformView: NSView {
    var peaks: [Float] = []  { didSet { needsDisplay = true } }
    var progress: Double = 0 { didSet { needsDisplay = true } }

    override func draw(_ dirty: NSRect) {
        let w = bounds.width, h = bounds.height
        if peaks.isEmpty {
            kTeal.withAlphaComponent(0.18).setFill()
            NSRect(x: 0, y: h/2 - 1, width: w, height: 2).fill()
            return
        }
        let barCount = peaks.count
        let barW     = w / CGFloat(barCount)
        let playX    = CGFloat(progress) * w

        for (i, peak) in peaks.enumerated() {
            let x    = CGFloat(i) * barW
            let barH = max(2, CGFloat(peak) * h * 0.88)
            let rect = NSRect(x: x + 0.5, y: (h - barH) / 2, width: max(1, barW - 1), height: barH)
            (x < playX ? kTeal : NSColor(white: 1, alpha: 0.14)).setFill()
            NSBezierPath(roundedRect: rect, xRadius: 1, yRadius: 1).fill()
        }
        // Playhead
        if progress > 0 {
            let path = NSBezierPath()
            path.move(to: NSPoint(x: playX, y: 0))
            path.line(to: NSPoint(x: playX, y: h))
            path.lineWidth = 1.5
            NSColor.white.withAlphaComponent(0.75).setStroke()
            path.stroke()
        }
    }
}

// MARK: - Audio analyzer

final class WMAudioAnalyzer {
    static let shared = WMAudioAnalyzer()
    private var cache: [String: [Float]] = [:]

    func analyze(_ url: URL, trackId: String, bars: Int = 120, completion: @escaping ([Float]) -> Void) {
        if let cached = cache[trackId] { completion(cached); return }
        DispatchQueue.global(qos: .userInitiated).async {
            guard let file   = try? AVAudioFile(forReading: url),
                  let format = AVAudioFormat(standardFormatWithSampleRate: file.fileFormat.sampleRate, channels: 1),
                  let buf    = AVAudioPCMBuffer(pcmFormat: format,
                                               frameCapacity: AVAudioFrameCount(file.length)),
                  (try? file.read(into: buf)) != nil,
                  let data   = buf.floatChannelData?[0] else {
                DispatchQueue.main.async { completion([]) }; return
            }
            let total = Int(buf.frameLength)
            let step  = max(1, total / bars)
            var peaks = (0..<bars).map { i -> Float in
                let start = i * step, end = min(start + step, total)
                return (start..<end).reduce(0) { max($0, abs(data[$1])) }
            }
            let mx = peaks.max() ?? 1
            if mx > 0 { peaks = peaks.map { $0 / mx } }
            DispatchQueue.main.async { self.cache[trackId] = peaks; completion(peaks) }
        }
    }
}

// MARK: - Custom NSTableView with drag support

final class WMTableView: NSTableView {
    var onDragRow: ((Int, NSEvent) -> Void)?

    override func draggingSession(_ session: NSDraggingSession,
                                  sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .copy }

    override func mouseDragged(with event: NSEvent) {
        let pt  = convert(event.locationInWindow, from: nil)
        let row = self.row(at: pt)
        guard row >= 0 else { return }
        onDragRow?(row, event)
    }
}

// MARK: - Track row view

final class WMTrackRow: NSTableCellView {
    private let playBtn    = NSButton(frame: .zero)
    private let nameLabel  = NSTextField(labelWithString: "")
    private let tagsLabel  = NSTextField(labelWithString: "")
    private let metaLabel  = NSTextField(labelWithString: "")
    let miniWave   = WMWaveformView()
    private let grip       = NSImageView()
    var onPlay: (() -> Void)?

    override init(frame: NSRect) { super.init(frame: frame); build() }
    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        wantsLayer = true
        layer?.backgroundColor = kRowBG.cgColor
        layer?.cornerRadius    = 6

        playBtn.isBordered       = false
        playBtn.imagePosition    = .imageOnly
        playBtn.target           = self
        playBtn.action           = #selector(tapPlay)
        playBtn.image            = NSImage(systemSymbolName: "play.circle", accessibilityDescription: nil)
        playBtn.contentTintColor = .tertiaryLabelColor

        nameLabel.font          = .systemFont(ofSize: 12, weight: .medium)
        nameLabel.textColor     = .labelColor
        nameLabel.lineBreakMode = .byTruncatingTail
        nameLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        tagsLabel.font          = .systemFont(ofSize: 10)
        tagsLabel.textColor     = NSColor(white: 1, alpha: 0.32)
        tagsLabel.lineBreakMode = .byTruncatingTail
        tagsLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        metaLabel.font          = .monospacedDigitSystemFont(ofSize: 9, weight: .regular)
        metaLabel.textColor     = .tertiaryLabelColor
        metaLabel.alignment     = .right
        metaLabel.setContentHuggingPriority(.required, for: .horizontal)

        grip.image            = NSImage(systemSymbolName: "line.3.horizontal", accessibilityDescription: nil)
        grip.contentTintColor = NSColor(white: 1, alpha: 0.15)
        grip.imageScaling     = .scaleProportionallyDown

        for v in [playBtn, nameLabel, tagsLabel, metaLabel, miniWave, grip] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }

        NSLayoutConstraint.activate([
            grip.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            grip.centerYAnchor.constraint(equalTo: centerYAnchor),
            grip.widthAnchor.constraint(equalToConstant: 13),
            grip.heightAnchor.constraint(equalToConstant: 13),

            miniWave.trailingAnchor.constraint(equalTo: grip.leadingAnchor, constant: -8),
            miniWave.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            miniWave.widthAnchor.constraint(equalToConstant: 68),
            miniWave.heightAnchor.constraint(equalToConstant: 18),

            metaLabel.trailingAnchor.constraint(equalTo: grip.leadingAnchor, constant: -8),
            metaLabel.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
            metaLabel.widthAnchor.constraint(equalToConstant: 68),

            playBtn.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            playBtn.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            playBtn.widthAnchor.constraint(equalToConstant: 20),
            playBtn.heightAnchor.constraint(equalToConstant: 20),

            nameLabel.leadingAnchor.constraint(equalTo: playBtn.trailingAnchor, constant: 8),
            nameLabel.topAnchor.constraint(equalTo: topAnchor, constant: 9),
            nameLabel.trailingAnchor.constraint(lessThanOrEqualTo: miniWave.leadingAnchor, constant: -6),

            tagsLabel.leadingAnchor.constraint(equalTo: nameLabel.leadingAnchor),
            tagsLabel.topAnchor.constraint(equalTo: nameLabel.bottomAnchor, constant: 2),
            tagsLabel.trailingAnchor.constraint(lessThanOrEqualTo: metaLabel.leadingAnchor, constant: -6),
        ])
    }

    func configure(track: WMTrack, playing: Bool, loadingAudio: Bool, downloadingDrag: Bool,
                   peaks: [Float], playProgress: Double) {
        nameLabel.stringValue = track.name

        // Tags: API returns " · "-separated string (e.g. "hip-hop · trap · drill")
        let tagParts = track.tags.components(separatedBy: " · ").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }.prefix(4)
        tagsLabel.stringValue = tagParts.joined(separator: " · ")
        tagsLabel.isHidden    = tagParts.isEmpty

        var metaParts: [String] = []
        if !track.displayTime.isEmpty { metaParts.append(track.displayTime) }
        if !track.key.isEmpty          { metaParts.append(track.displayKey) }
        metaLabel.stringValue = metaParts.joined(separator: "\n")

        let playIcon = loadingAudio ? "ellipsis.circle" : (playing ? "pause.circle.fill" : "play.circle")
        playBtn.image            = NSImage(systemSymbolName: playIcon, accessibilityDescription: nil)
        playBtn.contentTintColor = (playing || loadingAudio) ? kTeal : .tertiaryLabelColor

        miniWave.peaks    = peaks
        miniWave.progress = playing ? playProgress : 0
        miniWave.isHidden = peaks.isEmpty

        if downloadingDrag {
            grip.image            = NSImage(systemSymbolName: "arrow.down.circle", accessibilityDescription: nil)
            grip.contentTintColor = kTeal
        } else {
            grip.image            = NSImage(systemSymbolName: "line.3.horizontal", accessibilityDescription: nil)
            grip.contentTintColor = NSColor(white: 1, alpha: 0.15)
        }

        layer?.backgroundColor = (playing || loadingAudio)
            ? kTeal.withAlphaComponent(0.12).cgColor
            : kRowBG.cgColor
    }

    @objc private func tapPlay() { onPlay?() }
}

// MARK: - Library view controller (native, no WKWebView)

final class WMLibraryVC: NSViewController, NSTableViewDataSource, NSTableViewDelegate {

    // ── Sync header ──────────────────────────────────────────────────────────
    private let syncHeader    = NSView()
    private let syncDot       = NSView()
    private let syncStatusLbl = NSTextField(labelWithString: "NOT CONNECTED")
    private let syncDataLbl   = NSTextField(labelWithString: "")
    private let morphBtn      = NSButton(frame: .zero)
    private var isMorphed     = false

    // ── Genre pills ──────────────────────────────────────────────────────────
    private let pillsScroll   = NSScrollView()
    private let pillsStack    = NSStackView()
    private var activeGenre   = ""   // "" = All

    // ── Search ───────────────────────────────────────────────────────────────
    private let searchField   = NSSearchField()

    // ── Track list ───────────────────────────────────────────────────────────
    private let tableView     = WMTableView()
    private let scrollView    = NSScrollView()
    private let statusLabel   = NSTextField(labelWithString: "")

    // ── Player bar ───────────────────────────────────────────────────────────
    private let playerBar     = NSView()
    private let nowLabel      = NSTextField(labelWithString: "")
    private let timeLabel     = NSTextField(labelWithString: "")
    private let keyChip       = NSTextField(labelWithString: "")
    private let bpmChip       = NSTextField(labelWithString: "")
    private let waveformView  = WMWaveformView()
    private var playerBarH:   NSLayoutConstraint!

    // ── Feedback ─────────────────────────────────────────────────────────────
    private let feedbackBtn   = NSButton(frame: .zero)

    // ── Data ─────────────────────────────────────────────────────────────────
    private var allTracks:      [WMTrack] = []
    private var filtered:       [WMTrack] = []
    private var downloadingIds: Set<String> = []
    private var cachedIds:      Set<String> = []
    private var waveformCache:  [String: [Float]] = [:]
    private var currentProgress: Double = 0

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 380, height: 640))
        view.wantsLayer = true
        view.layer?.backgroundColor = kBG.cgColor
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        buildFeedbackBtn()   // must be first — playerBar anchors to it
        buildPlayerBar()
        buildSyncHeader()
        buildGenrePills()
        buildSearchBar()
        buildTable()
        buildStatus()
        connectAudioPlayer()
        loadLibrary()

        WMSyncState.shared.onChange = { [weak self] in
            DispatchQueue.main.async { self?.updateSyncUI() }
        }
        updateSyncUI()
        startLogicAudioWatcher()
    }

    // MARK: Logic audio auto-detect
    // Polls lsof every 2s to find audio files Logic has open.
    // When a new file is found, writes its path to /tmp/water-morph-analyze-file.txt
    // so the plugin's timer picks it up and runs BRAIN analysis automatically.

    private var logicWatchTimer: Timer?
    private var lastAnalyzedAudioPath = ""

    private func startLogicAudioWatcher() {
        logicWatchTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.checkLogicAudioFile()
        }
    }

    private func checkLogicAudioFile() {
        DispatchQueue.global(qos: .background).async { [weak self] in
            guard let self else { return }
            // Find Logic Pro PID
            var mib = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
            var size = 0
            sysctl(&mib, 4, nil, &size, nil, 0)

            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/bin/sh")
            task.arguments = ["-c",
                "LPID=$(pgrep -x 'Logic Pro' | head -1); " +
                "[ -z \"$LPID\" ] && exit 0; " +
                "lsof -F n -p \"$LPID\" 2>/dev/null | sed -n 's/^n//p' | " +
                "grep -iE '\\.(wav|aiff|aif|mp3|m4a)$' | " +
                "grep -v '/System\\|/Library/Audio\\|/Applications\\|\\.component' | " +
                "head -1"]
            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError  = Pipe()
            guard (try? task.run()) != nil else { return }
            task.waitUntilExit()
            let path = String(data: pipe.fileHandleForReading.readDataToEndOfFile(),
                              encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !path.isEmpty, path != self.lastAnalyzedAudioPath else { return }
            self.lastAnalyzedAudioPath = path
            // Write to IPC file — plugin's timerCallback picks it up
            try? path.write(toFile: "/tmp/water-morph-analyze-file.txt",
                            atomically: true, encoding: .utf8)
        }
    }

    // MARK: Build — feedback

    private func buildFeedbackBtn() {
        feedbackBtn.title            = "Send feedback"
        feedbackBtn.isBordered       = false
        feedbackBtn.font             = .systemFont(ofSize: 11)
        feedbackBtn.contentTintColor = NSColor(white: 0.28, alpha: 1)
        feedbackBtn.target           = self
        feedbackBtn.action           = #selector(openFeedback)
        feedbackBtn.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(feedbackBtn)
        NSLayoutConstraint.activate([
            feedbackBtn.bottomAnchor.constraint(equalTo: view.bottomAnchor, constant: -4),
            feedbackBtn.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            feedbackBtn.heightAnchor.constraint(equalToConstant: 24),
        ])
    }

    // MARK: Build — player bar

    private func buildPlayerBar() {
        playerBar.wantsLayer = true
        playerBar.layer?.backgroundColor = NSColor(white: 0.09, alpha: 1).cgColor
        playerBar.isHidden = true
        playerBar.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(playerBar)

        let line = NSView(); line.wantsLayer = true
        line.layer?.backgroundColor = kTeal.withAlphaComponent(0.3).cgColor
        line.translatesAutoresizingMaskIntoConstraints = false
        playerBar.addSubview(line)

        func ctrl(_ sym: String, action: Selector) -> NSButton {
            let b = NSButton(frame: .zero); b.isBordered = false; b.imagePosition = .imageOnly
            b.image = NSImage(systemSymbolName: sym, accessibilityDescription: nil)
            b.contentTintColor = .secondaryLabelColor; b.target = self; b.action = action
            b.translatesAutoresizingMaskIntoConstraints = false; return b
        }
        let stopBtn = ctrl("stop.circle",   action: #selector(stopAudio))
        let prevBtn = ctrl("backward.fill", action: #selector(prevTrack))
        let nextBtn = ctrl("forward.fill",  action: #selector(nextTrack))
        stopBtn.contentTintColor = .tertiaryLabelColor

        nowLabel.font = .systemFont(ofSize: 11, weight: .semibold)
        nowLabel.textColor = .secondaryLabelColor; nowLabel.lineBreakMode = .byTruncatingTail
        nowLabel.translatesAutoresizingMaskIntoConstraints = false

        func chip() -> NSTextField {
            let f = NSTextField(labelWithString: "")
            f.font = .systemFont(ofSize: 10, weight: .semibold); f.textColor = kTeal
            f.wantsLayer = true; f.layer?.backgroundColor = kTeal.withAlphaComponent(0.12).cgColor
            f.layer?.cornerRadius = 4; f.translatesAutoresizingMaskIntoConstraints = false
            return f
        }
        let kc = chip(); kc.translatesAutoresizingMaskIntoConstraints = false
        let bc = chip(); bc.translatesAutoresizingMaskIntoConstraints = false
        // wire to instance vars via assignment below
        playerBar.addSubview(line); playerBar.addSubview(stopBtn)
        playerBar.addSubview(prevBtn); playerBar.addSubview(nextBtn)
        playerBar.addSubview(nowLabel); playerBar.addSubview(kc)
        playerBar.addSubview(bc); playerBar.addSubview(timeLabel)
        playerBar.addSubview(waveformView)

        timeLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
        timeLabel.textColor = .tertiaryLabelColor; timeLabel.alignment = .right
        timeLabel.setContentHuggingPriority(.required, for: .horizontal)
        timeLabel.translatesAutoresizingMaskIntoConstraints = false
        waveformView.translatesAutoresizingMaskIntoConstraints = false

        // Wire chip references
        keyChip.font = kc.font; keyChip.textColor = kc.textColor
        keyChip.wantsLayer = true; keyChip.layer?.backgroundColor = kTeal.withAlphaComponent(0.12).cgColor
        keyChip.layer?.cornerRadius = 4; keyChip.translatesAutoresizingMaskIntoConstraints = false
        bpmChip.font = bc.font; bpmChip.textColor = bc.textColor
        bpmChip.wantsLayer = true; bpmChip.layer?.backgroundColor = kTeal.withAlphaComponent(0.12).cgColor
        bpmChip.layer?.cornerRadius = 4; bpmChip.translatesAutoresizingMaskIntoConstraints = false

        playerBarH = playerBar.heightAnchor.constraint(equalToConstant: 84)

        NSLayoutConstraint.activate([
            playerBar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            playerBar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            playerBar.bottomAnchor.constraint(equalTo: feedbackBtn.topAnchor),
            playerBarH,

            line.topAnchor.constraint(equalTo: playerBar.topAnchor),
            line.leadingAnchor.constraint(equalTo: playerBar.leadingAnchor),
            line.trailingAnchor.constraint(equalTo: playerBar.trailingAnchor),
            line.heightAnchor.constraint(equalToConstant: 1),

            stopBtn.leadingAnchor.constraint(equalTo: playerBar.leadingAnchor, constant: 10),
            stopBtn.topAnchor.constraint(equalTo: playerBar.topAnchor, constant: 10),
            stopBtn.widthAnchor.constraint(equalToConstant: 16),
            stopBtn.heightAnchor.constraint(equalToConstant: 16),

            nextBtn.trailingAnchor.constraint(equalTo: playerBar.trailingAnchor, constant: -10),
            nextBtn.centerYAnchor.constraint(equalTo: stopBtn.centerYAnchor),
            nextBtn.widthAnchor.constraint(equalToConstant: 18), nextBtn.heightAnchor.constraint(equalToConstant: 18),

            prevBtn.trailingAnchor.constraint(equalTo: nextBtn.leadingAnchor, constant: -10),
            prevBtn.centerYAnchor.constraint(equalTo: stopBtn.centerYAnchor),
            prevBtn.widthAnchor.constraint(equalToConstant: 18), prevBtn.heightAnchor.constraint(equalToConstant: 18),

            nowLabel.leadingAnchor.constraint(equalTo: stopBtn.trailingAnchor, constant: 7),
            nowLabel.centerYAnchor.constraint(equalTo: stopBtn.centerYAnchor),
            nowLabel.trailingAnchor.constraint(lessThanOrEqualTo: prevBtn.leadingAnchor, constant: -8),

            kc.leadingAnchor.constraint(equalTo: playerBar.leadingAnchor, constant: 10),
            kc.topAnchor.constraint(equalTo: stopBtn.bottomAnchor, constant: 6),

            bc.leadingAnchor.constraint(equalTo: kc.trailingAnchor, constant: 5),
            bc.centerYAnchor.constraint(equalTo: kc.centerYAnchor),

            timeLabel.trailingAnchor.constraint(equalTo: playerBar.trailingAnchor, constant: -10),
            timeLabel.centerYAnchor.constraint(equalTo: kc.centerYAnchor),

            waveformView.topAnchor.constraint(equalTo: kc.bottomAnchor, constant: 6),
            waveformView.leadingAnchor.constraint(equalTo: playerBar.leadingAnchor, constant: 10),
            waveformView.trailingAnchor.constraint(equalTo: playerBar.trailingAnchor, constant: -10),
            waveformView.bottomAnchor.constraint(equalTo: playerBar.bottomAnchor, constant: -8),
        ])
    }

    // MARK: Build — sync header

    private func buildSyncHeader() {
        syncHeader.wantsLayer = true
        syncHeader.layer?.backgroundColor = NSColor(white: 0.055, alpha: 1).cgColor
        syncHeader.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(syncHeader)

        syncDot.wantsLayer = true; syncDot.layer?.cornerRadius = 4
        syncDot.layer?.backgroundColor = NSColor(white: 0.22, alpha: 1).cgColor
        syncDot.translatesAutoresizingMaskIntoConstraints = false

        syncStatusLbl.font = .monospacedSystemFont(ofSize: 9.5, weight: .semibold)
        syncStatusLbl.textColor = NSColor(white: 0.28, alpha: 1)
        syncStatusLbl.translatesAutoresizingMaskIntoConstraints = false

        syncDataLbl.font = .monospacedSystemFont(ofSize: 12.5, weight: .bold)
        syncDataLbl.textColor = kTeal
        syncDataLbl.translatesAutoresizingMaskIntoConstraints = false

        morphBtn.title = "✦"; morphBtn.isBordered = false
        morphBtn.font = .systemFont(ofSize: 15)
        morphBtn.contentTintColor = NSColor(white: 0.25, alpha: 1)
        morphBtn.toolTip = "Morph library to DAW BPM & key"
        morphBtn.isEnabled = false
        morphBtn.target = self; morphBtn.action = #selector(morphTapped)
        morphBtn.translatesAutoresizingMaskIntoConstraints = false

        let hairline = NSView(); hairline.wantsLayer = true
        hairline.layer?.backgroundColor = NSColor(white: 0.11, alpha: 1).cgColor
        hairline.translatesAutoresizingMaskIntoConstraints = false

        for v in [syncDot, syncStatusLbl, syncDataLbl, morphBtn, hairline] as [NSView] {
            syncHeader.addSubview(v)
        }

        NSLayoutConstraint.activate([
            syncHeader.topAnchor.constraint(equalTo: view.topAnchor),
            syncHeader.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            syncHeader.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            syncHeader.heightAnchor.constraint(equalToConstant: 40),

            syncDot.leadingAnchor.constraint(equalTo: syncHeader.leadingAnchor, constant: 12),
            syncDot.centerYAnchor.constraint(equalTo: syncHeader.centerYAnchor),
            syncDot.widthAnchor.constraint(equalToConstant: 8),
            syncDot.heightAnchor.constraint(equalToConstant: 8),

            syncStatusLbl.leadingAnchor.constraint(equalTo: syncDot.trailingAnchor, constant: 6),
            syncStatusLbl.centerYAnchor.constraint(equalTo: syncHeader.centerYAnchor),

            syncDataLbl.centerXAnchor.constraint(equalTo: syncHeader.centerXAnchor),
            syncDataLbl.centerYAnchor.constraint(equalTo: syncHeader.centerYAnchor),

            morphBtn.trailingAnchor.constraint(equalTo: syncHeader.trailingAnchor, constant: -10),
            morphBtn.centerYAnchor.constraint(equalTo: syncHeader.centerYAnchor),
            morphBtn.widthAnchor.constraint(equalToConstant: 28),
            morphBtn.heightAnchor.constraint(equalToConstant: 28),

            hairline.bottomAnchor.constraint(equalTo: syncHeader.bottomAnchor),
            hairline.leadingAnchor.constraint(equalTo: syncHeader.leadingAnchor),
            hairline.trailingAnchor.constraint(equalTo: syncHeader.trailingAnchor),
            hairline.heightAnchor.constraint(equalToConstant: 1),
        ])
    }

    // MARK: Build — genre pills

    private static let kGenres = ["All", "Melody", "Beats", "Toplines", "Songs"]

    private func buildGenrePills() {
        pillsScroll.hasHorizontalScroller = false
        pillsScroll.hasVerticalScroller   = false
        pillsScroll.drawsBackground       = false
        pillsScroll.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(pillsScroll)

        pillsStack.orientation  = .horizontal
        pillsStack.spacing      = 6
        pillsStack.translatesAutoresizingMaskIntoConstraints = false

        for genre in Self.kGenres { pillsStack.addArrangedSubview(makePillBtn(genre)) }

        let clip = NSClipView(); clip.documentView = pillsStack
        pillsScroll.contentView = clip

        NSLayoutConstraint.activate([
            pillsScroll.topAnchor.constraint(equalTo: syncHeader.bottomAnchor),
            pillsScroll.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            pillsScroll.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            pillsScroll.heightAnchor.constraint(equalToConstant: 38),

            pillsStack.topAnchor.constraint(equalTo: clip.topAnchor, constant: 7),
            pillsStack.leadingAnchor.constraint(equalTo: clip.leadingAnchor, constant: 10),
        ])
    }

    private func makePillBtn(_ title: String) -> NSButton {
        let isActive = (title == "All" && activeGenre.isEmpty) || title == activeGenre
        let btn = NSButton(title: title, target: self, action: #selector(pillTapped(_:)))
        btn.identifier = NSUserInterfaceItemIdentifier(title)
        btn.bezelStyle = .rounded; btn.isBordered = false; btn.wantsLayer = true
        btn.layer?.cornerRadius = 11; btn.layer?.borderWidth = 1
        btn.font = .systemFont(ofSize: 11, weight: isActive ? .semibold : .regular)
        btn.contentTintColor = isActive ? kTeal : NSColor(white: 0.45, alpha: 1)
        btn.layer?.backgroundColor = isActive ? kTeal.withAlphaComponent(0.18).cgColor : NSColor(white: 0.11, alpha: 1).cgColor
        btn.layer?.borderColor     = isActive ? kTeal.withAlphaComponent(0.6).cgColor  : NSColor(white: 0.18, alpha: 1).cgColor
        btn.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            btn.heightAnchor.constraint(equalToConstant: 24),
            btn.widthAnchor.constraint(greaterThanOrEqualToConstant: 52),
        ])
        return btn
    }

    @objc private func pillTapped(_ sender: NSButton) {
        let genre = sender.identifier?.rawValue ?? "All"
        activeGenre = (genre == "All") ? "" : genre
        for pill in pillsStack.arrangedSubviews.compactMap({ $0 as? NSButton }) {
            let pg = pill.identifier?.rawValue ?? "All"
            let on = (pg == "All" && activeGenre.isEmpty) || pg == activeGenre
            pill.layer?.backgroundColor = on ? kTeal.withAlphaComponent(0.18).cgColor : NSColor(white: 0.11, alpha: 1).cgColor
            pill.layer?.borderColor     = on ? kTeal.withAlphaComponent(0.6).cgColor  : NSColor(white: 0.18, alpha: 1).cgColor
            pill.contentTintColor       = on ? kTeal : NSColor(white: 0.45, alpha: 1)
            pill.font = .systemFont(ofSize: 11, weight: on ? .semibold : .regular)
        }
        applyFilter()
    }

    // MARK: Build — search

    private func buildSearchBar() {
        searchField.placeholderString = "Search tracks…"
        searchField.controlSize       = .regular
        searchField.target            = self
        searchField.action            = #selector(onSearch)
        searchField.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(searchField)

        NSLayoutConstraint.activate([
            searchField.topAnchor.constraint(equalTo: pillsScroll.bottomAnchor, constant: 4),
            searchField.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 8),
            searchField.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -8),
        ])
    }

    // MARK: Build — track table

    private func buildTable() {
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("col"))
        col.resizingMask = .autoresizingMask
        tableView.addTableColumn(col)
        tableView.headerView              = nil
        tableView.rowHeight               = 48
        tableView.backgroundColor         = .clear
        tableView.selectionHighlightStyle = .none
        tableView.intercellSpacing        = NSSize(width: 0, height: 4)
        tableView.dataSource              = self
        tableView.delegate                = self
        tableView.onDragRow               = { [weak self] row, event in self?.startDrag(row: row, event: event) }

        scrollView.documentView       = tableView
        scrollView.hasVerticalScroller = true
        scrollView.drawsBackground    = false
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)

        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 6),
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 4),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -4),
            scrollView.bottomAnchor.constraint(equalTo: playerBar.topAnchor, constant: -4),
        ])
    }

    // MARK: Build — status label

    private func buildStatus() {
        statusLabel.font      = .systemFont(ofSize: 12)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.alignment = .center
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(statusLabel)
        NSLayoutConstraint.activate([
            statusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            statusLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
        ])
    }

    // MARK: Sync UI

    private func updateSyncUI() {
        let s         = WMSyncState.shared
        let connected = s.isConnected

        syncDot.layer?.backgroundColor = (connected ? kTeal : NSColor(white: 0.22, alpha: 1)).cgColor
        syncStatusLbl.stringValue = connected ? "" : "NOT CONNECTED"
        syncStatusLbl.textColor   = NSColor(white: 0.28, alpha: 1)

        syncDataLbl.stringValue = connected ? "\(Int(s.bpm)) BPM · \(s.keyLabel)" : ""
        syncDataLbl.textColor   = isMorphed ? kMauve : kTeal

        morphBtn.isEnabled        = connected
        morphBtn.contentTintColor = isMorphed ? kMauve : (connected ? kTeal.withAlphaComponent(0.7) : NSColor(white: 0.25, alpha: 1))
        morphBtn.toolTip          = isMorphed ? "✓ Morphed" : (connected ? "Morph to \(Int(s.bpm)) BPM · \(s.keyLabel)" : "Connect Morph plugin first")

        if !connected { isMorphed = false }
    }

    @objc private func morphTapped() {
        guard WMSyncState.shared.isConnected else { return }
        isMorphed.toggle()
        updateSyncUI()
        if isMorphed {
            let sync = WMSyncState.shared
            for t in allTracks where cachedIds.contains(t.id) && t.bpm > 0 {
                preMorph(t, sync: sync)
            }
        }
    }

    private func preMorph(_ track: WMTrack, sync: WMSyncState) {
        let tgt   = sync.bpm
        let semis = semitonesBetween(from: track.key, to: sync.key)
        let key   = "morph-\(track.id)"
        let outName = WMMorphEngine.morphedName(trackName: track.name, bpm: tgt, semitones: semis)
        let outURL  = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(outName)
        guard !FileManager.default.fileExists(atPath: outURL.path),
              !downloadingIds.contains(key) else { return }
        downloadingIds.insert(key)
        let input = track.cachedFile
        DispatchQueue.global(qos: .background).async { [weak self] in
            _ = try? WMMorphEngine.render(input: input, sourceBPM: track.bpm,
                                          targetBPM: tgt, semitoneDelta: semis,
                                          trackName: track.name)
            DispatchQueue.main.async { self?.downloadingIds.remove(key) }
        }
    }

    @objc private func openFeedback() {
        if let url = URL(string: "mailto:hello@95ent.ai?subject=Morph%20Feedback") {
            NSWorkspace.shared.open(url)
        }
    }

    // MARK: Audio player

    private func connectAudioPlayer() {
        WMAudioPlayer.shared.onStateChange = { [weak self] in
            guard let self else { return }
            let id = WMAudioPlayer.shared.currentId
            let playing = !id.isEmpty
            if let t = self.allTracks.first(where: { $0.id == id }) {
                self.nowLabel.stringValue = t.name
                self.nowLabel.textColor   = kTeal
                self.keyChip.stringValue  = t.key.isEmpty ? "" : " \(t.displayKey) "
                self.bpmChip.stringValue  = t.bpm <= 0    ? "" : " \(t.displayBPM) bpm "
                self.keyChip.isHidden     = t.key.isEmpty
                self.bpmChip.isHidden     = t.bpm <= 0
                self.triggerAnalysis(t)
            } else {
                self.keyChip.isHidden      = true
                self.bpmChip.isHidden      = true
                self.waveformView.peaks    = []
                self.waveformView.progress = 0
                self.timeLabel.stringValue = ""
                self.currentProgress       = 0
            }
            // Show/hide player bar
            if playing != !self.playerBar.isHidden {
                self.playerBar.isHidden        = !playing
                self.playerBarH.constant       = playing ? 84 : 0
                self.view.layoutSubtreeIfNeeded()
            }
            self.tableView.reloadData()
        }

        WMAudioPlayer.shared.onProgressChange = { [weak self] progress, currentTime in
            guard let self else { return }
            self.currentProgress       = progress
            self.waveformView.progress = progress
            let id = WMAudioPlayer.shared.currentId
            if let idx = self.filtered.firstIndex(where: { $0.id == id }),
               let cell = self.tableView.view(atColumn: 0, row: idx, makeIfNecessary: false) as? WMTrackRow {
                cell.miniWave.progress = progress
            }
            let total = WMAudioPlayer.shared.currentDuration
            self.timeLabel.stringValue = total > 0
                ? "\(Self.fmt(currentTime)) / \(Self.fmt(total))"
                : Self.fmt(currentTime)
        }
    }

    private func triggerAnalysis(_ t: WMTrack) {
        guard FileManager.default.fileExists(atPath: t.cachedFile.path) else { waveformView.peaks = []; return }
        if let cached = waveformCache[t.id] { waveformView.peaks = cached; return }
        WMAudioAnalyzer.shared.analyze(t.cachedFile, trackId: t.id) { [weak self] peaks in
            guard let self else { return }
            self.waveformCache[t.id] = peaks
            if WMAudioPlayer.shared.currentId == t.id { self.waveformView.peaks = peaks }
            if let idx = self.filtered.firstIndex(where: { $0.id == t.id }) {
                self.tableView.reloadData(forRowIndexes: IndexSet(integer: idx), columnIndexes: IndexSet(integer: 0))
            }
        }
    }

    private static func fmt(_ t: Double) -> String {
        let s = Int(t); return "\(s / 60):\(String(format: "%02d", s % 60))"
    }

    @objc private func prevTrack() {
        let id = WMAudioPlayer.shared.currentId
        guard let idx = filtered.firstIndex(where: { $0.id == id }), idx > 0 else { return }
        play(filtered[idx - 1])
    }

    @objc private func nextTrack() {
        let id = WMAudioPlayer.shared.currentId
        guard let idx = filtered.firstIndex(where: { $0.id == id }), idx < filtered.count - 1 else { return }
        play(filtered[idx + 1])
    }

    // MARK: Data

    private func loadLibrary() {
        statusLabel.stringValue = "Loading…"
        statusLabel.isHidden    = false
        // Refresh token first (mirrors old WMWebLibraryVC behaviour)
        let rt = WMTokenStore.shared.refreshToken
        if !rt.isEmpty {
            WMAPIClient.shared.refreshTokens { [weak self] _ in self?.fetchTracks() }
        } else {
            fetchTracks()
        }
    }

    private func fetchTracks() {
        WMAPIClient.shared.fetchAllTracks { [weak self] ok, tracks in
            guard let self else { return }
            self.allTracks = tracks
            self.applyFilter()
            self.statusLabel.isHidden = !tracks.isEmpty
            if tracks.isEmpty {
                self.statusLabel.stringValue = ok ? "Your library is empty." : "Could not load — check connection."
            }
            for t in tracks where FileManager.default.fileExists(atPath: t.cachedFile.path) {
                self.cachedIds.insert(t.id)
                WMAudioAnalyzer.shared.analyze(t.cachedFile, trackId: t.id) { [weak self] peaks in
                    guard let self else { return }
                    self.waveformCache[t.id] = peaks
                    if let idx = self.filtered.firstIndex(where: { $0.id == t.id }) {
                        self.tableView.reloadData(forRowIndexes: IndexSet(integer: idx), columnIndexes: IndexSet(integer: 0))
                    }
                }
            }
        }
    }

    private func applyFilter() {
        var result = allTracks
        if !activeGenre.isEmpty {
            result = result.filter { $0.tags.lowercased().contains(activeGenre.lowercased()) }
        }
        let q = searchField.stringValue.lowercased().trimmingCharacters(in: .whitespaces)
        if !q.isEmpty {
            result = result.filter {
                $0.name.lowercased().contains(q) || $0.key.lowercased().contains(q) ||
                $0.tags.lowercased().contains(q) || $0.displayBPM.contains(q)
            }
        }
        filtered = result
        tableView.reloadData()
    }

    @objc private func onSearch() { applyFilter() }
    @objc private func stopAudio() { WMAudioPlayer.shared.stop() }

    // MARK: Play

    private func play(_ track: WMTrack) {
        WMAudioPlayer.shared.toggle(track)
        prefetchIfNeeded(track)
        tableView.reloadData()
    }

    private func prefetchIfNeeded(_ track: WMTrack) {
        guard !cachedIds.contains(track.id), !downloadingIds.contains(track.id) else { return }
        downloadingIds.insert(track.id)
        tableView.reloadData()
        WMAPIClient.shared.downloadForDrag(track) { [weak self] url in
            guard let self else { return }
            self.downloadingIds.remove(track.id)
            if url != nil {
                self.cachedIds.insert(track.id)
                // If morphed, immediately kick off background render for this track
                if self.isMorphed && track.bpm > 0 {
                    self.preMorph(track, sync: WMSyncState.shared)
                }
            }
            self.tableView.reloadData()
        }
    }

    // MARK: Drag to DAW — delivers morphed WAV when Morph is active

    private func startDrag(row: Int, event: NSEvent) {
        guard row < filtered.count else { return }
        let track = filtered[row]

        guard cachedIds.contains(track.id),
              FileManager.default.fileExists(atPath: track.cachedFile.path) else {
            prefetchIfNeeded(track); return
        }

        let sync   = WMSyncState.shared
        let pbItem = NSPasteboardItem()

        if isMorphed && sync.isConnected && track.bpm > 0 {
            let semis = semitonesBetween(from: track.key, to: sync.key)
            let label = WMMorphEngine.displayName(trackName: track.name, bpm: sync.bpm, key: sync.key)
            pbItem.setDataProvider(
                WMMorphDragProvider(track: track, targetBPM: sync.bpm, semitoneDelta: semis, displayName: label),
                forTypes: [.fileURL])
        } else {
            pbItem.setDataProvider(WMDragFileProvider(track), forTypes: [.fileURL])
        }

        let dragItem = NSDraggingItem(pasteboardWriter: pbItem)
        let rowRect  = tableView.rect(ofRow: row)
        let icon: NSImage
        if #available(macOS 11.0, *) { icon = NSWorkspace.shared.icon(for: UTType.audio) }
        else { icon = NSWorkspace.shared.icon(forFileType: "mp3") }
        icon.size = NSSize(width: 32, height: 32)
        dragItem.setDraggingFrame(NSRect(x: rowRect.midX - 16, y: rowRect.midY - 16, width: 32, height: 32),
                                  contents: icon)
        tableView.beginDraggingSession(with: [dragItem], event: event, source: tableView)
    }

    // MARK: NSTableViewDataSource / Delegate

    func numberOfRows(in tableView: NSTableView) -> Int { filtered.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id   = NSUserInterfaceItemIdentifier("row")
        let cell = tableView.makeView(withIdentifier: id, owner: nil) as? WMTrackRow ?? WMTrackRow(frame: .zero)
        cell.identifier = id
        let track     = filtered[row]
        let isPlaying = WMAudioPlayer.shared.currentId == track.id && WMAudioPlayer.shared.isPlaying
        cell.configure(track: track,
                       playing:         isPlaying,
                       loadingAudio:    WMAudioPlayer.shared.isLoadingId == track.id,
                       downloadingDrag: downloadingIds.contains(track.id),
                       peaks:           waveformCache[track.id] ?? [],
                       playProgress:    isPlaying ? currentProgress : 0)
        cell.onPlay = { [weak self] in self?.play(track) }
        return cell
    }

    func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat { 56 }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        let v = NSTableRowView(); v.wantsLayer = true
        v.layer?.backgroundColor = NSColor.clear.cgColor
        return v
    }
}

// MARK: - Login view controller

final class WMLoginVC: NSViewController {
    private let logoLabel   = NSTextField(labelWithString: "Morph")
    private let subLabel    = NSTextField(labelWithString: "by Water")
    private let loginBtn    = NSButton(title: "Sign In with Water", target: nil, action: nil)
    private let statusLabel = NSTextField(labelWithString: "")
    private var pollTimer:  Timer?
    private var sessionId   = ""

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 420))
        view.wantsLayer = true
        view.layer?.backgroundColor = kBG.cgColor
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        logoLabel.font      = .systemFont(ofSize: 28, weight: .bold)
        logoLabel.textColor = .labelColor
        logoLabel.alignment = .center

        subLabel.font      = .systemFont(ofSize: 13)
        subLabel.textColor = kTeal
        subLabel.alignment = .center

        loginBtn.bezelStyle     = .rounded
        loginBtn.keyEquivalent  = "\r"
        loginBtn.target         = self
        loginBtn.action         = #selector(startLogin)

        statusLabel.font      = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.alignment = .center
        statusLabel.lineBreakMode = .byWordWrapping

        for v in [logoLabel, subLabel, loginBtn, statusLabel] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(v)
        }
        NSLayoutConstraint.activate([
            logoLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            logoLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor, constant: -60),

            subLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            subLabel.topAnchor.constraint(equalTo: logoLabel.bottomAnchor, constant: 4),

            loginBtn.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            loginBtn.topAnchor.constraint(equalTo: subLabel.bottomAnchor, constant: 28),
            loginBtn.widthAnchor.constraint(equalToConstant: 200),

            statusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            statusLabel.topAnchor.constraint(equalTo: loginBtn.bottomAnchor, constant: 14),
            statusLabel.widthAnchor.constraint(equalToConstant: 260),
        ])
    }

    @objc private func startLogin() {
        loginBtn.isEnabled  = false
        statusLabel.stringValue = "Connecting…"
        WMAPIClient.shared.authStart { [weak self] sid in
            guard let self, let sid else {
                self?.statusLabel.stringValue = "Could not reach server. Check your connection."
                self?.loginBtn.isEnabled = true
                return
            }
            self.sessionId = sid
            NSWorkspace.shared.open(URL(string: "\(kBaseURL)/plugin-auth?session_id=\(sid)")!)
            self.statusLabel.stringValue = "Waiting for browser sign-in…"
            self.pollTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
                self?.poll()
            }
        }
    }

    private func poll() {
        WMAPIClient.shared.authPoll(sessionId) { [weak self] access, refresh in
            guard let self, let access, !access.isEmpty else { return }
            self.pollTimer?.invalidate(); self.pollTimer = nil
            WMTokenStore.shared.accessToken  = access
            WMTokenStore.shared.refreshToken = refresh ?? ""
            WMTokenStore.shared.save()
            (self.view.window?.windowController as? WMWindowController)?.showLibrary()
        }
    }
}

// MARK: - WKScriptMessageHandler weak proxy (avoids retain cycle)

private final class WKMessageProxy: NSObject, WKScriptMessageHandler {
    weak var target: WKScriptMessageHandler?
    init(_ t: WKScriptMessageHandler) { target = t }
    func userContentController(_ uc: WKUserContentController, didReceive msg: WKScriptMessage) {
        target?.userContentController(uc, didReceive: msg)
    }
}

// MARK: - Lazy drag file provider

/// Delivers the mp3 to the drop target on demand, blocking until the download
/// completes. AppKit calls this on an indeterminate (non-main) thread — safe to block.
final class WMDragFileProvider: NSObject, NSPasteboardItemDataProvider {
    private let track: WMTrack
    init(_ track: WMTrack) { self.track = track; super.init() }

    func pasteboard(_ pasteboard: NSPasteboard?,
                    item: NSPasteboardItem,
                    provideDataForType type: NSPasteboard.PasteboardType) {
        guard type == .fileURL else { return }
        let dest = track.cachedFile
        if !FileManager.default.fileExists(atPath: dest.path) {
            let sem = DispatchSemaphore(value: 0)
            WMAPIClient.shared.downloadForDrag(track) { _ in sem.signal() }
            _ = sem.wait(timeout: .now() + 30)
        }
        guard FileManager.default.fileExists(atPath: dest.path) else { return }

        // Build a human-readable filename: "Track Name (Gbm 144bpm).mp3"
        let safeName = track.name
            .replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
            .replacingOccurrences(of: "\"", with: "")
        let keyPart  = track.key.isEmpty  ? "" : " \(track.key)"
        let bpmPart  = track.bpm > 0      ? " \(Int(track.bpm))bpm" : ""
        let label    = keyPart.isEmpty && bpmPart.isEmpty ? "" : " (\(keyPart.trimmingCharacters(in: .whitespaces))\(bpmPart))"
        let fileName = "\(safeName)\(label).mp3"
        let named    = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(fileName)
        try? FileManager.default.removeItem(at: named)
        try? FileManager.default.copyItem(at: dest, to: named)
        let final = FileManager.default.fileExists(atPath: named.path) ? named : dest
        item.setString(final.absoluteString, forType: .fileURL)
    }

    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {}
}

// MARK: - Offline morph engine (time-stretch + pitch-shift via AVAudioEngine)

final class WMMorphEngine {
    enum MorphError: Error { case fileNotFound, renderFailed }

    /// Canonical filename for a morphed render. No double-dash for negative semitones.
    static func morphedName(trackName: String, bpm: Double, semitones: Int) -> String {
        let safe = trackName
            .replacingOccurrences(of: " ", with: "_")
            .replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
            .replacingOccurrences(of: "\"", with: "")
        let semiStr = semitones >= 0 ? "\(semitones)" : "m\(abs(semitones))"
        return "morphed-\(Int(bpm))bpm-\(semiStr)st-\(safe).wav"
    }

    // Human-readable filename that Logic/Finder will show: "Baby No More [Bmin 91bpm].wav"
    static func displayName(trackName: String, bpm: Double, key: String) -> String {
        let keyStr = key.isEmpty ? "" : "\(key) "
        return "\(trackName) [\(keyStr)\(Int(bpm))bpm].wav"
    }

    /// Renders a time-stretched + pitch-shifted copy of `input`.
    /// Returns the output WAV URL. Runs synchronously — call from a background thread.
    static func render(input: URL,
                       sourceBPM: Double,
                       targetBPM: Double,
                       semitoneDelta: Int,
                       trackName: String = "") throws -> URL {
        guard FileManager.default.fileExists(atPath: input.path) else {
            throw MorphError.fileNotFound
        }

        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let tp     = AVAudioUnitTimePitch()

        let ratio   = max(0.25, min(4.0, targetBPM / sourceBPM))
        tp.rate     = Float(ratio)
        tp.pitch    = Float(semitoneDelta * 100)  // cents

        engine.attach(player)
        engine.attach(tp)

        guard let file = try? AVAudioFile(forReading: input) else {
            throw MorphError.fileNotFound
        }
        let fmt = file.processingFormat
        engine.connect(player, to: tp, format: fmt)
        engine.connect(tp, to: engine.mainMixerNode, format: fmt)

        try engine.enableManualRenderingMode(.offline, format: fmt, maximumFrameCount: 4096)
        try engine.start()

        player.scheduleFile(file, at: nil)
        player.play()

        let resolvedName = trackName.isEmpty
            ? input.deletingPathExtension().lastPathComponent.replacingOccurrences(of: "morph-", with: "")
            : trackName
        let outName = WMMorphEngine.morphedName(trackName: resolvedName, bpm: targetBPM, semitones: semitoneDelta)
        let outURL   = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(outName)
        let settings: [String: Any] = [
            AVFormatIDKey:         kAudioFormatLinearPCM,
            AVSampleRateKey:       fmt.sampleRate,
            AVNumberOfChannelsKey: fmt.channelCount,
            AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false,
        ]
        let outFile = try AVAudioFile(forWriting: outURL, settings: settings)
        let totalOut = AVAudioFrameCount(Double(file.length) / ratio) + 4096
        let buf      = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: 4096)!
        var written: AVAudioFrameCount = 0

        var stuckCount = 0
        while written < totalOut {
            let frames = min(4096, totalOut - written)
            buf.frameLength = frames
            let st = try engine.renderOffline(frames, to: buf)
            if st == .insufficientDataFromInputNode { break }
            if st == .cannotDoInCurrentContext {
                // AVAudioUnitTimePitch warmup — skip without advancing position.
                stuckCount += 1
                if stuckCount > 128 { throw MorphError.renderFailed }
                continue
            }
            stuckCount = 0
            if buf.frameLength > 0 { try outFile.write(from: buf) }
            written += buf.frameLength
        }
        engine.stop()

        // Validate: a real audio file must be larger than a bare WAV header.
        let size = (try? FileManager.default.attributesOfItem(atPath: outURL.path))?[FileAttributeKey.size] as? Int ?? 0
        guard size > 8192 else { throw MorphError.renderFailed }

        return outURL
    }
}

// Delivers a pre-named raw file (copy to display-name temp file)
final class WMNamedDragProvider: NSObject, NSPasteboardItemDataProvider {
    private let source: URL
    private let displayName: String
    init(source: URL, displayName: String) { self.source = source; self.displayName = displayName; super.init() }

    func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem,
                    provideDataForType type: NSPasteboard.PasteboardType) {
        guard type == .fileURL else { return }
        let named = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(displayName)
        try? FileManager.default.removeItem(at: named)
        try? FileManager.default.copyItem(at: source, to: named)
        let final = FileManager.default.fileExists(atPath: named.path) ? named : source
        item.setString(final.absoluteString, forType: .fileURL)
    }
    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {}
}

// Delivers a morphed WAV (blocks on render; called from non-main thread by AppKit)
final class WMMorphDragProvider: NSObject, NSPasteboardItemDataProvider {
    private let track: WMTrack
    private let targetBPM: Double
    private let semitoneDelta: Int
    private let displayName: String

    init(track: WMTrack, targetBPM: Double, semitoneDelta: Int, displayName: String) {
        self.track = track; self.targetBPM = targetBPM
        self.semitoneDelta = semitoneDelta; self.displayName = displayName
        super.init()
    }

    func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem,
                    provideDataForType type: NSPasteboard.PasteboardType) {
        guard type == .fileURL else { return }
        let src = track.cachedFile
        // Ensure raw file is available
        if !FileManager.default.fileExists(atPath: src.path) {
            let sem = DispatchSemaphore(value: 0)
            WMAPIClient.shared.downloadForDrag(track) { _ in sem.signal() }
            _ = sem.wait(timeout: .now() + 30)
        }
        guard FileManager.default.fileExists(atPath: src.path) else { return }

        // Check for pre-rendered morph (background render started on hover).
        // Validate size > 8KB — a failed render leaves an empty WAV header.
        let preRenderName = WMMorphEngine.morphedName(trackName: track.name, bpm: targetBPM, semitones: semitoneDelta)
        let preRenderURL  = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(preRenderName)
        let preSize = (try? FileManager.default.attributesOfItem(atPath: preRenderURL.path))?[FileAttributeKey.size] as? Int ?? 0
        if preSize > 8192 {
            // Copy to display-name path so Logic receives "Baby No More [Bmin 91bpm].wav"
            let namedURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(displayName)
            if preRenderURL.path != namedURL.path {
                try? FileManager.default.removeItem(at: namedURL)
                try? FileManager.default.copyItem(at: preRenderURL, to: namedURL)
            }
            let final = FileManager.default.fileExists(atPath: namedURL.path) ? namedURL : preRenderURL
            item.setString(final.absoluteString, forType: .fileURL)
            return
        }

        // Render now (on-demand, blocks until done)
        if let morphed = try? WMMorphEngine.render(input: src,
                                                    sourceBPM: track.bpm > 0 ? track.bpm : targetBPM,
                                                    targetBPM: targetBPM,
                                                    semitoneDelta: semitoneDelta,
                                                    trackName: track.name) {
            // Copy to display-name path so Logic receives "Baby No More [Bmin 91bpm].wav"
            let namedURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(displayName)
            if morphed.path != namedURL.path {
                try? FileManager.default.removeItem(at: namedURL)
                try? FileManager.default.copyItem(at: morphed, to: namedURL)
            }
            let final = FileManager.default.fileExists(atPath: namedURL.path) ? namedURL : morphed
            item.setString(final.absoluteString, forType: .fileURL)
        } else {
            // Morph failed — fall back to raw file with display name
            let named = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(displayName)
            try? FileManager.default.removeItem(at: named)
            try? FileManager.default.copyItem(at: src, to: named)
            let final = FileManager.default.fileExists(atPath: named.path) ? named : src
            item.setString(final.absoluteString, forType: .fileURL)
        }
    }
    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {}
}

// Semitone distance between two key strings ("Dmin", "Cmaj", etc.)
func semitonesBetween(from: String, to: String) -> Int {
    let noteOrder = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
    func noteIndex(_ key: String) -> Int? {
        let name = key.replacingOccurrences(of: "min", with: "")
                      .replacingOccurrences(of: "maj", with: "")
                      .replacingOccurrences(of: "m", with: "")
                      .trimmingCharacters(in: .whitespaces)
        return noteOrder.firstIndex(of: name)
    }
    guard let a = noteIndex(from), let b = noteIndex(to) else { return 0 }
    var diff = b - a
    if diff > 6 { diff -= 12 }
    if diff < -6 { diff += 12 }
    return diff
}

// MARK: - WKWebView subclass for native DAW drag

final class MorphWebView: WKWebView, NSDraggingSource {
    var hoveredId = ""
    var onNativeDrag: ((String, NSEvent) -> Void)?
    private var mouseDownPt: NSPoint = .zero
    private let kDragThreshold: CGFloat = 8

    func draggingSession(_ session: NSDraggingSession,
                         sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .copy }

    override func mouseDown(with event: NSEvent) {
        mouseDownPt = event.locationInWindow
        super.mouseDown(with: event)
    }

    override func mouseDragged(with event: NSEvent) {
        if !hoveredId.isEmpty {
            let dx = event.locationInWindow.x - mouseDownPt.x
            let dy = event.locationInWindow.y - mouseDownPt.y
            if sqrt(dx*dx + dy*dy) >= kDragThreshold {
                // Don't start DAW drag when mouseDown originated in the player bar
                // (bottom ~65px of the webView — player bar is ~56px tall).
                // That zone uses pointer capture for KEY/BPM knob drag.
                let localDown = convert(mouseDownPt, from: nil)
                guard localDown.y >= 65 else {
                    super.mouseDragged(with: event)
                    return
                }
                onNativeDrag?(hoveredId, event)
                return
            }
        }
        super.mouseDragged(with: event)
    }
}

// MARK: - Draggable selection bar button

final class WMDragButton: NSView, NSDraggingSource {
    var onDrag: ((NSEvent) -> Void)?
    private var downPt: NSPoint = .zero
    private let kThresh: CGFloat = 6

    func draggingSession(_ session: NSDraggingSession,
                         sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .copy }
    override func mouseDown(with event: NSEvent) { downPt = event.locationInWindow }
    override func mouseDragged(with event: NSEvent) {
        let dx = event.locationInWindow.x - downPt.x
        let dy = event.locationInWindow.y - downPt.y
        if sqrt(dx*dx + dy*dy) >= kThresh { onDrag?(event) }
    }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// MARK: - Web library view controller (WKWebView, Splice-style: Water UI as-is)

final class WMWebLibraryVC: NSViewController, WKNavigationDelegate, WKScriptMessageHandler {

    private var webView: MorphWebView!
    private var syncBar: NSView!
    private var navBar: NSView!
    private var navBackBtn: NSButton!
    private var navFwdBtn: NSButton!
    private var navTabBtns: [NSButton] = []
    private let kNavTabs: [(label: String, path: String, icon: String)] = [
        ("Discover",    "/discover",    "binoculars"),
        ("Library",     "/library",     "house"),
        ("Favorites",   "/favorites",   "heart"),
        ("Collections", "/collections", "music.note.list"),
    ]
    private var syncDot: NSView!
    private var syncModeLabel: NSTextField!
    private var syncBpmLabel: NSTextField!
    private var syncKeyLabel: NSTextField!
    private var morphBtn: NSButton!
    private var selBar: NSView!
    private var selBarH: NSLayoutConstraint!
    private var selLabel: NSTextField!
    private var selEditBtn: NSButton!
    private var selDragBtn: WMDragButton!
    private var feedbackStrip: NSView!
    private var loadingOverlay: NSView?
    private var isMorphed = false
    private var trackMap: [String: WMTrack] = [:]
    private var cachedIds: Set<String> = []
    private var prefetchingIds: Set<String> = []
    private var morphingIds: Set<String> = []
    private var selectedIds: Set<String> = [] {
        didSet { updateSelectionBar() }
    }

    override func loadView() {
        let config = WKWebViewConfiguration()
        config.userContentController.add(WKMessageProxy(self), name: "morphHover")
        config.userContentController.add(WKMessageProxy(self), name: "morphSelect")
        config.userContentController.add(WKMessageProxy(self), name: "morphCopy")
        config.userContentController.add(WKMessageProxy(self), name: "morphPlayerState")
        // Auth UserScript is injected dynamically in loadWebUI() so it always
        // uses the latest (possibly refreshed) tokens, never a stale snapshot.

        webView = MorphWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = self
        webView.allowsBackForwardNavigationGestures = false
        webView.underPageBackgroundColor = kBG
        webView.onNativeDrag = { [weak self] id, event in self?.handleDrag(id, event) }
        // Must include "WaterMorphCompanion" so CompanionBridge activates
        // window.__waterCompanion in the React app (setLock / clearLock bridge).
        webView.customUserAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) WaterMorphCompanion/1.0"

        // Container: [syncBar 40px] [webView fills] [selBar 44px hidden]
        let container = NSView(frame: .zero)

        // ── Sync header bar ──────────────────────────────────────────────────
        syncBar = NSView(frame: .zero)
        syncBar.wantsLayer = true
        syncBar.layer?.backgroundColor = NSColor(white: 0.075, alpha: 1).cgColor

        // Connection dot
        syncDot = NSView(frame: .zero)
        syncDot.wantsLayer = true
        syncDot.layer?.cornerRadius = 4
        syncDot.layer?.backgroundColor = NSColor(white: 0.25, alpha: 1).cgColor
        syncDot.translatesAutoresizingMaskIntoConstraints = false

        // Mode label (PROJECT / TRACK / NOT CONNECTED)
        syncModeLabel = NSTextField(labelWithString: "NOT CONNECTED")
        syncModeLabel.font = .monospacedSystemFont(ofSize: 9, weight: .semibold)
        syncModeLabel.textColor = .init(white: 0.3, alpha: 1)
        syncModeLabel.translatesAutoresizingMaskIntoConstraints = false

        // BPM value
        syncBpmLabel = NSTextField(labelWithString: "—")
        syncBpmLabel.font = .monospacedSystemFont(ofSize: 13, weight: .bold)
        syncBpmLabel.textColor = .init(white: 0.25, alpha: 1)
        syncBpmLabel.translatesAutoresizingMaskIntoConstraints = false

        // Separator + KEY
        syncKeyLabel = NSTextField(labelWithString: "—")
        syncKeyLabel.font = .monospacedSystemFont(ofSize: 13, weight: .bold)
        syncKeyLabel.textColor = .init(white: 0.25, alpha: 1)
        syncKeyLabel.translatesAutoresizingMaskIntoConstraints = false

        // ── morphBtn — lives in syncBar (reads DAW BPM/key, sends to React) ──
        morphBtn = NSButton(title: "✦  Morph", target: self, action: #selector(onMorphTapped))
        morphBtn.isBordered = false
        morphBtn.wantsLayer = true
        morphBtn.layer?.cornerRadius = 14
        morphBtn.layer?.backgroundColor = NSColor(white: 0.12, alpha: 1).cgColor
        morphBtn.layer?.borderColor = kTeal.withAlphaComponent(0.4).cgColor
        morphBtn.layer?.borderWidth = 1
        morphBtn.contentTintColor = kTeal
        morphBtn.font = .systemFont(ofSize: 11, weight: .semibold)
        morphBtn.translatesAutoresizingMaskIntoConstraints = false

        syncBar.addSubview(syncDot)
        syncBar.addSubview(syncModeLabel)
        syncBar.addSubview(syncBpmLabel)
        syncBar.addSubview(syncKeyLabel)
        syncBar.addSubview(morphBtn)
        syncBar.translatesAutoresizingMaskIntoConstraints = false

        webView.translatesAutoresizingMaskIntoConstraints = false

        // ── Native nav bar — Splice-style top chrome ─────────────────────────
        navBar = NSView(frame: .zero)
        navBar.wantsLayer = true
        navBar.layer?.backgroundColor = NSColor(white: 0.039, alpha: 1).cgColor
        navBar.translatesAutoresizingMaskIntoConstraints = false

        // Back / forward — SF Symbol chevrons, image-only
        let makeArrow: (String, Selector) -> NSButton = { sym, action in
            let btn = NSButton(frame: .zero)
            btn.isBordered = false
            btn.imagePosition = .imageOnly
            let cfg = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
            btn.image = NSImage(systemSymbolName: sym, accessibilityDescription: nil)?
                .withSymbolConfiguration(cfg)
            btn.contentTintColor = NSColor(white: 0.28, alpha: 1)
            btn.target = self; btn.action = action
            btn.translatesAutoresizingMaskIntoConstraints = false
            return btn
        }
        navBackBtn = makeArrow("chevron.left",  #selector(navBack))
        navFwdBtn  = makeArrow("chevron.right", #selector(navForward))
        navBar.addSubview(navBackBtn)
        navBar.addSubview(navFwdBtn)

        // Tab buttons — icon above label (matches mobile bottom nav)
        let inactiveCfg = NSImage.SymbolConfiguration(pointSize: 16, weight: .regular)
        for (idx, tab) in kNavTabs.enumerated() {
            let btn = NSButton(frame: .zero)
            btn.isBordered = false
            btn.wantsLayer = true
            btn.imagePosition = .imageAbove
            btn.image = NSImage(systemSymbolName: tab.icon, accessibilityDescription: tab.label)?
                .withSymbolConfiguration(inactiveCfg)
            btn.title = tab.label
            btn.font  = .systemFont(ofSize: 9, weight: .medium)
            btn.contentTintColor = NSColor(white: 0.35, alpha: 1)
            btn.tag    = idx
            btn.target = self; btn.action = #selector(navTabTapped(_:))
            btn.translatesAutoresizingMaskIntoConstraints = false
            navTabBtns.append(btn)
            navBar.addSubview(btn)
        }

        // Tabs fill the space to the right of the arrows via an equal-width stack
        let tabStack = NSStackView(views: navTabBtns)
        tabStack.orientation  = .horizontal
        tabStack.distribution = .fillEqually
        tabStack.spacing      = 0
        tabStack.translatesAutoresizingMaskIntoConstraints = false
        navBar.addSubview(tabStack)

        // Bottom separator line
        let sep = NSView(frame: .zero)
        sep.wantsLayer = true
        sep.layer?.backgroundColor = NSColor(white: 1, alpha: 0.06).cgColor
        sep.translatesAutoresizingMaskIntoConstraints = false
        navBar.addSubview(sep)

        NSLayoutConstraint.activate([
            navBackBtn.leadingAnchor.constraint(equalTo: navBar.leadingAnchor, constant: 6),
            navBackBtn.centerYAnchor.constraint(equalTo: navBar.centerYAnchor),
            navBackBtn.widthAnchor.constraint(equalToConstant: 24),
            navBackBtn.heightAnchor.constraint(equalToConstant: 24),

            navFwdBtn.leadingAnchor.constraint(equalTo: navBackBtn.trailingAnchor, constant: 2),
            navFwdBtn.centerYAnchor.constraint(equalTo: navBar.centerYAnchor),
            navFwdBtn.widthAnchor.constraint(equalToConstant: 24),
            navFwdBtn.heightAnchor.constraint(equalToConstant: 24),

            tabStack.leadingAnchor.constraint(equalTo: navFwdBtn.trailingAnchor, constant: 4),
            tabStack.trailingAnchor.constraint(equalTo: navBar.trailingAnchor),
            tabStack.topAnchor.constraint(equalTo: navBar.topAnchor),
            tabStack.bottomAnchor.constraint(equalTo: navBar.bottomAnchor, constant: -1),

            sep.leadingAnchor.constraint(equalTo: navBar.leadingAnchor),
            sep.trailingAnchor.constraint(equalTo: navBar.trailingAnchor),
            sep.bottomAnchor.constraint(equalTo: navBar.bottomAnchor),
            sep.heightAnchor.constraint(equalToConstant: 1),
        ])

        container.addSubview(syncBar)
        container.addSubview(navBar)
        container.addSubview(webView)

        // Selection bar (bottom, hidden until tracks selected)
        selBar = NSView(frame: .zero)
        selBar.wantsLayer = true
        selBar.layer?.backgroundColor = NSColor(red: 0.102, green: 0.565, blue: 0.627, alpha: 0.15).cgColor
        selBar.translatesAutoresizingMaskIntoConstraints = false
        selBar.isHidden = true

        selLabel = NSTextField(labelWithString: "")
        selLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        selLabel.textColor = kTeal
        selLabel.translatesAutoresizingMaskIntoConstraints = false

        selDragBtn = WMDragButton(frame: .zero)
        selDragBtn.wantsLayer = true
        selDragBtn.layer?.backgroundColor = kTeal.withAlphaComponent(0.2).cgColor
        selDragBtn.layer?.cornerRadius = 6
        selDragBtn.translatesAutoresizingMaskIntoConstraints = false
        selDragBtn.toolTip = "Drag tracks to DAW"
        selDragBtn.onDrag = { [weak self] event in self?.handleMultiDrag(event) }

        let dragLabel = NSTextField(labelWithString: "⇥ Drag to DAW")
        dragLabel.font = .systemFont(ofSize: 11, weight: .medium)
        dragLabel.textColor = kTeal
        dragLabel.isEditable = false
        dragLabel.isBordered = false
        dragLabel.backgroundColor = .clear
        dragLabel.translatesAutoresizingMaskIntoConstraints = false
        selDragBtn.addSubview(dragLabel)

        selEditBtn = NSButton(frame: .zero)
        selEditBtn.isBordered = false
        selEditBtn.wantsLayer = true
        selEditBtn.layer?.backgroundColor = kTeal.withAlphaComponent(0.15).cgColor
        selEditBtn.layer?.cornerRadius = 6
        selEditBtn.title = "✎  Edit"
        selEditBtn.font = .systemFont(ofSize: 11, weight: .medium)
        selEditBtn.contentTintColor = kTeal
        selEditBtn.toolTip = "Bulk-edit selected tracks"
        selEditBtn.target = self
        selEditBtn.action = #selector(onSelEditTapped)
        selEditBtn.translatesAutoresizingMaskIntoConstraints = false

        selBar.addSubview(selLabel)
        selBar.addSubview(selEditBtn)
        selBar.addSubview(selDragBtn)
        container.addSubview(selBar)

        // ── Feedback strip — must be created before constraints reference it ──
        feedbackStrip = NSView(frame: .zero)
        feedbackStrip.wantsLayer = true
        feedbackStrip.layer?.backgroundColor = NSColor(white: 0.04, alpha: 1).cgColor
        feedbackStrip.translatesAutoresizingMaskIntoConstraints = false

        let fbSep = NSView(frame: .zero)
        fbSep.wantsLayer = true
        fbSep.layer?.backgroundColor = NSColor(white: 1, alpha: 0.05).cgColor
        fbSep.translatesAutoresizingMaskIntoConstraints = false

        let fbBtn = NSButton(frame: .zero)
        fbBtn.title = "Send feedback"
        fbBtn.isBordered = false
        fbBtn.font = .systemFont(ofSize: 10.5)
        fbBtn.contentTintColor = kTeal
        fbBtn.target = self
        fbBtn.action = #selector(openFeedbackForm)
        fbBtn.translatesAutoresizingMaskIntoConstraints = false
        feedbackStrip.addSubview(fbSep)
        feedbackStrip.addSubview(fbBtn)
        container.addSubview(feedbackStrip)

        NSLayoutConstraint.activate([
            // Sync bar (40px — DAW status only)
            syncBar.topAnchor.constraint(equalTo: container.topAnchor),
            syncBar.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            syncBar.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            syncBar.heightAnchor.constraint(equalToConstant: 40),

            syncDot.leadingAnchor.constraint(equalTo: syncBar.leadingAnchor, constant: 12),
            syncDot.centerYAnchor.constraint(equalTo: syncBar.centerYAnchor),
            syncDot.widthAnchor.constraint(equalToConstant: 8),
            syncDot.heightAnchor.constraint(equalToConstant: 8),

            syncModeLabel.leadingAnchor.constraint(equalTo: syncDot.trailingAnchor, constant: 6),
            syncModeLabel.centerYAnchor.constraint(equalTo: syncBar.centerYAnchor),

            syncBpmLabel.centerXAnchor.constraint(equalTo: syncBar.centerXAnchor, constant: -18),
            syncBpmLabel.centerYAnchor.constraint(equalTo: syncBar.centerYAnchor),

            syncKeyLabel.leadingAnchor.constraint(equalTo: syncBpmLabel.trailingAnchor, constant: 8),
            syncKeyLabel.centerYAnchor.constraint(equalTo: syncBar.centerYAnchor),

            // Nav bar (52px — back/fwd + icon+label tabs)
            navBar.topAnchor.constraint(equalTo: syncBar.bottomAnchor),
            navBar.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            navBar.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            navBar.heightAnchor.constraint(equalToConstant: 52),

            // Web view — fills all space between navBar and selBar
            webView.topAnchor.constraint(equalTo: navBar.bottomAnchor),
            webView.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            webView.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            webView.bottomAnchor.constraint(equalTo: selBar.topAnchor),

            // morphBtn in syncBar — right side pill
            morphBtn.trailingAnchor.constraint(equalTo: syncBar.trailingAnchor, constant: -10),
            morphBtn.centerYAnchor.constraint(equalTo: syncBar.centerYAnchor),
            morphBtn.heightAnchor.constraint(equalToConstant: 28),
            morphBtn.widthAnchor.constraint(equalToConstant: 72),
        ])

        // selBar height stored so updateSelectionBar can collapse it to 0 when hidden
        selBarH = selBar.heightAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            selBar.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            selBar.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            selBar.bottomAnchor.constraint(equalTo: feedbackStrip.topAnchor),
            selBarH,

            selLabel.leadingAnchor.constraint(equalTo: selBar.leadingAnchor, constant: 14),
            selLabel.centerYAnchor.constraint(equalTo: selBar.centerYAnchor),

            selDragBtn.trailingAnchor.constraint(equalTo: selBar.trailingAnchor, constant: -12),
            selDragBtn.centerYAnchor.constraint(equalTo: selBar.centerYAnchor),
            selDragBtn.heightAnchor.constraint(equalToConstant: 28),
            selDragBtn.widthAnchor.constraint(equalToConstant: 110),

            dragLabel.centerXAnchor.constraint(equalTo: selDragBtn.centerXAnchor),
            dragLabel.centerYAnchor.constraint(equalTo: selDragBtn.centerYAnchor),

            selEditBtn.trailingAnchor.constraint(equalTo: selDragBtn.leadingAnchor, constant: -8),
            selEditBtn.centerYAnchor.constraint(equalTo: selBar.centerYAnchor),
            selEditBtn.heightAnchor.constraint(equalToConstant: 28),
            selEditBtn.widthAnchor.constraint(equalToConstant: 70),
        ])

        // feedbackStrip created above (before constraints)

        NSLayoutConstraint.activate([
            feedbackStrip.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            feedbackStrip.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            feedbackStrip.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            feedbackStrip.heightAnchor.constraint(equalToConstant: 26),

            fbSep.topAnchor.constraint(equalTo: feedbackStrip.topAnchor),
            fbSep.leadingAnchor.constraint(equalTo: feedbackStrip.leadingAnchor),
            fbSep.trailingAnchor.constraint(equalTo: feedbackStrip.trailingAnchor),
            fbSep.heightAnchor.constraint(equalToConstant: 1),

            fbBtn.centerXAnchor.constraint(equalTo: feedbackStrip.centerXAnchor),
            fbBtn.centerYAnchor.constraint(equalTo: feedbackStrip.centerYAnchor),
        ])

        // Dark overlay — covers the webview until auth is confirmed.
        // Prevents the web app's empty-library drop zone from flashing before login.
        let overlay = NSView(frame: .zero)
        overlay.wantsLayer = true
        overlay.layer?.backgroundColor = kBG.cgColor
        overlay.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(overlay)
        NSLayoutConstraint.activate([
            overlay.topAnchor.constraint(equalTo: container.topAnchor),
            overlay.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            overlay.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            overlay.bottomAnchor.constraint(equalTo: container.bottomAnchor),
        ])
        loadingOverlay = overlay

        view = container
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        fetchTrackMap()
        let rt = WMTokenStore.shared.refreshToken
        if rt.isEmpty {
            loadWebUI()
        } else {
            WMAPIClient.shared.refreshTokens { [weak self] _ in self?.loadWebUI() }
        }
        WMSyncState.shared.onChange = { [weak self] in
            guard let self else { return }
            self.updateSyncUI()
        }
        updateSyncUI()
        // KVO: keep nav bar active state in sync during client-side route changes
        webView.addObserver(self, forKeyPath: #keyPath(WKWebView.url), options: .new, context: nil)
    }

    deinit {
        webView?.removeObserver(self, forKeyPath: #keyPath(WKWebView.url))
    }

    override func observeValue(forKeyPath keyPath: String?, of object: Any?,
                               change: [NSKeyValueChangeKey: Any]?, context: UnsafeMutableRawPointer?) {
        if keyPath == #keyPath(WKWebView.url) {
            DispatchQueue.main.async { [weak self] in self?.updateNavBar() }
        }
    }

    // MARK: Sync UI

    private func updateSyncUI() {
        let s = WMSyncState.shared
        let connected = s.isConnected

        // Connection dot: teal when live, dim when not
        syncDot.layer?.backgroundColor = (connected ? kTeal : NSColor(white: 0.22, alpha: 1)).cgColor

        // Mode label
        syncModeLabel.stringValue = connected ? "" : "NOT CONNECTED"
        syncModeLabel.textColor   = .init(white: 0.28, alpha: 1)

        // BPM — mauve when morphed, teal when connected, dim otherwise
        syncBpmLabel.stringValue = connected ? "\(Int(s.bpm)) BPM" : "—"
        syncBpmLabel.textColor   = isMorphed ? kMauve : (connected ? kTeal : .init(white: 0.28, alpha: 1))

        // KEY — mauve when morphed, white when connected, dim otherwise
        syncKeyLabel.stringValue = connected ? "· \(s.keyLabel)" : ""
        syncKeyLabel.textColor   = isMorphed ? kMauve : .init(white: connected ? 0.85 : 0.28, alpha: 1)

        // Morph pill button (lives in syncBar)
        if connected && isMorphed {
            morphBtn.title = "✦  Morphed"
            morphBtn.contentTintColor = .white
            morphBtn.layer?.backgroundColor = kMauve.cgColor
            morphBtn.layer?.borderColor = kMauve.cgColor
        } else if connected {
            morphBtn.title = "✦  Morph"
            morphBtn.contentTintColor = kTeal
            morphBtn.layer?.backgroundColor = NSColor(white: 0.12, alpha: 1).cgColor
            morphBtn.layer?.borderColor = kTeal.withAlphaComponent(0.4).cgColor
        } else {
            morphBtn.title = "✦  Morph"
            morphBtn.contentTintColor = NSColor(white: 0.3, alpha: 1)
            morphBtn.layer?.backgroundColor = NSColor(white: 0.10, alpha: 1).cgColor
            morphBtn.layer?.borderColor = NSColor(white: 0.2, alpha: 1).cgColor
        }
        morphBtn.isEnabled = connected

        // Reset morphed state on disconnect
        if isMorphed && !connected {
            isMorphed = false
        }
    }

    // MARK: Nav bar

    @objc private func navBack() { webView.goBack() }
    @objc private func navForward() { webView.goForward() }

    @objc private func navTabTapped(_ sender: NSButton) {
        let path = kNavTabs[sender.tag].path
        // Client-side navigation: click the hidden Next.js Link so React state
        // (Zustand audio player) is preserved — no full page reload.
        let js = """
        (function(){
          var a=document.querySelector('[data-bottom-nav] a[href="\(path)"]');
          if(a){a.click();return;}
          history.pushState({},'','\(path)');
          window.dispatchEvent(new PopStateEvent('popstate',{state:{}}));
        })();
        """
        webView.evaluateJavaScript(js) { [weak self] _, _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
                self?.updateNavBar()
            }
        }
    }

    private func updateNavBar() {
        navBackBtn.contentTintColor = webView.canGoBack
            ? NSColor(white: 0.75, alpha: 1)
            : NSColor(white: 0.22, alpha: 1)
        navFwdBtn.contentTintColor = webView.canGoForward
            ? NSColor(white: 0.75, alpha: 1)
            : NSColor(white: 0.22, alpha: 1)

        guard let path = webView.url?.path else { return }
        let activeCfg   = NSImage.SymbolConfiguration(pointSize: 17, weight: .semibold)
        let inactiveCfg = NSImage.SymbolConfiguration(pointSize: 16, weight: .regular)
        for (i, tab) in kNavTabs.enumerated() {
            let active = path.hasPrefix(tab.path)
            let btn = navTabBtns[i]
            btn.contentTintColor = active ? kTeal : NSColor(white: 0.35, alpha: 1)
            btn.font  = .systemFont(ofSize: 9, weight: active ? .semibold : .medium)
            btn.image = NSImage(systemSymbolName: tab.icon, accessibilityDescription: tab.label)?
                .withSymbolConfiguration(active ? activeCfg : inactiveCfg)
        }
    }

    // MARK: Morph action

    @objc private func onMorphTapped() {
        guard WMSyncState.shared.isConnected else { return }
        isMorphed.toggle()
        updateSyncUI()

        let bpm = Int(WMSyncState.shared.bpm)
        let key = WMSyncState.shared.key

        if isMorphed {
            applyMorphLock(bpm: bpm, key: key, attempt: 0)
            for id in cachedIds { preMorphIfNeeded(id) }
        } else {
            let js = """
            (function(){
              var s=document.getElementById('__wm_morph_active');
              if(s) s.remove();
              var bar=document.querySelector('[data-wm-morph-bar]');
              if(bar){bar.removeAttribute('data-wm-morph-bar');bar.style.borderTopColor='';bar.style.boxShadow='';}
              if(window.__waterCompanion){ window.__waterCompanion.clearLock(); }
              window.postMessage({type:'MORPH_INACTIVE'},\'*\');
            })();
            """
            webView.evaluateJavaScript(js, completionHandler: nil)
        }
    }

    // Retries up to 5× (500 ms apart) until window.__waterCompanion is ready.
    // Needed because React hydration can finish after didFinish fires.
    private func applyMorphLock(bpm: Int, key: String, attempt: Int) {
        let js = """
        (function(){
          if(!window.__waterCompanion){
            return 'NOT_READY';
          }
          // Style injection: BPM/key text + track titles turn mauve
          var s=document.getElementById('__wm_morph_active');
          if(s) s.remove();
          s=document.createElement('style');
          s.id='__wm_morph_active';
          s.textContent='[class*="bpm"]:not(button):not(input):not(label),[class*="key"]:not(button):not(input):not(label):not([class*="keyboard"]){color:#8B5CF6!important;transition:color 0.35s ease!important;}';
          document.head.appendChild(s);
          // Purple glow on player bar border-top
          (function(){
            var canv=document.querySelector('canvas.absolute.inset-0');
            if(!canv) return;
            var el=canv;
            while(el&&el!==document.body){
              if(el.classList&&el.classList.contains('overflow-hidden')&&el.classList.contains('border-t')){
                el.setAttribute('data-wm-morph-bar','1');
                el.style.borderTopColor='rgba(139,92,246,0.8)';
                el.style.boxShadow='0 -4px 20px rgba(139,92,246,0.28)';
                break;
              }
              el=el.parentElement;
            }
          })();
          // Lock Signalsmith to project BPM + Key
          window.__waterCompanion.setLock('\(key)',\(bpm));
          window.postMessage({type:'MORPH_ACTIVE',bpm:\(bpm),key:'\(key)'},\'*\');
          return 'OK';
        })();
        """
        webView.evaluateJavaScript(js) { [weak self] result, _ in
            guard let self else { return }
            if (result as? String) == "NOT_READY" && attempt < 5 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    self.applyMorphLock(bpm: bpm, key: key, attempt: attempt + 1)
                }
            }
        }
    }

    // MARK: Auth injection

    // Generates localStorage + document.cookie injection JS using the CURRENT
    // WMTokenStore values (always fresh at call time).
    private func buildAuthJS() -> String {
        let access  = WMTokenStore.shared.accessToken
        let refresh = WMTokenStore.shared.refreshToken
        var userId = "", email = ""
        let parts = access.split(separator: ".")
        if parts.count >= 2 {
            var b64 = String(parts[1])
                .replacingOccurrences(of: "-", with: "+")
                .replacingOccurrences(of: "_", with: "/")
            let rem = b64.count % 4
            if rem > 0 { b64 += String(repeating: "=", count: 4 - rem) }
            if let data = Data(base64Encoded: b64),
               let p = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                userId = (p["sub"]   as? String ?? "").replacingOccurrences(of: "'", with: "\\'")
                email  = (p["email"] as? String ?? "").replacingOccurrences(of: "'", with: "\\'")
            }
        }
        let exp = Int(Date().timeIntervalSince1970) + 3600
        let a = access.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "'", with: "\\'")
        let r = refresh.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "'", with: "\\'")
        // Always overwrite — never skip if a stale session exists in localStorage.
        return """
        (function(){
          // Suppress hydration-error noise in the companion WKWebView.
          // The WKWebView viewport (400 px) differs from the SSR assumed viewport,
          // which triggers React #418 (text content mismatch). The library still
          // renders correctly — we just silence the console + Sentry toast.
          var _ce=console.error;
          console.error=function(){
            var m=Array.prototype.join.call(arguments,' ');
            if(m.indexOf('418')!==-1||m.indexOf('Hydration')!==-1||m.indexOf('hydration')!==-1) return;
            _ce.apply(console,arguments);
          };
          try {
            var k='sb-hlqwvctfmxljjmosfcqq-auth-token';
            var v=JSON.stringify({
              access_token:'\(a)',token_type:'bearer',
              expires_in:3600,expires_at:\(exp),
              refresh_token:'\(r)',
              user:{id:'\(userId)',aud:'authenticated',role:'authenticated',
                    email:'\(email)',email_confirmed_at:'2024-01-01T00:00:00Z',
                    app_metadata:{provider:'email',providers:['email']},
                    user_metadata:{},created_at:'2024-01-01T00:00:00Z',
                    updated_at:'2024-01-01T00:00:00Z'}
            });
            localStorage.setItem(k,v);
          } catch(e){}
        })();
        """
    }

    // CSS injected at documentStart — hides desktop chrome before first paint.
    private func buildCompanionCSSScript() -> String {
        let css = """
        #nav-bar { display: none !important; }
        [data-bottom-nav] { display: none !important; }
        [data-genre-picker] { display: none !important; }
        [data-collection-header] { display: none !important; }
        [data-mobile-spacer] { display: none !important; }
        .fixed.inset-0.z-\\[200\\] { display: none !important; }
        .fixed.bottom-24, .fixed.bottom-20 { display: none !important; }
        h1.pl-3 { display: none !important; }
        html, body, #__next { background: #0d0d0d !important; }
        [data-greeting] { text-align: center !important; font-size: 11px !important; color: rgba(255,255,255,0.25) !important; }
        [data-tabs-bar] { justify-content: center !important; }
        [data-inline-bpm-panel] { display: none !important; }
        .shrink-0[aria-hidden="true"] { display: none !important; }
        [data-pending-overlay] { display: none !important; }
        /* Tags — plain muted text, no pill (Splice-style) */
        button.rounded-full { font-size: 9.5px !important; padding: 2px 5px !important; }
        button.rounded-full:not([style]) { color: rgba(255,255,255,0.28) !important; border-color: transparent !important; background-color: transparent !important; }
        button.rounded-full[style] { background-color: transparent !important; border-color: transparent !important; font-weight: 600 !important; }
        /* Filter chips row — scroll from left, slightly bigger pills */
        [data-filter-row] { justify-content: flex-start !important; }
        [data-filter-row] button { height: 32px !important; font-size: 11.5px !important; padding: 0 12px !important; }
        """
        return """
        (function(){
          if(document.getElementById('__morphCSS')) return;
          var s=document.createElement('style');
          s.id='__morphCSS';
          s.textContent=`\(css)`;
          document.documentElement.appendChild(s);
        })();
        """
    }

    // Patch AudioWorklet.addModule so blob: URLs are converted to data: URLs.
    // WKWebView blocks blob: URLs for AudioWorklet modules (both online and
    // OfflineAudioContext). Signalsmith registers its processor via blob URL,
    // causing renderDerivative to produce silence in the companion and
    // triggering the low-quality soundtouch-ts fallback.
    private func buildAudioWorkletPatchScript() -> String {
        return """
        (function(){
          if(window.__wkAudioWorkletPatched) return;
          window.__wkAudioWorkletPatched = true;
          const _orig = AudioWorklet.prototype.addModule;
          AudioWorklet.prototype.addModule = async function(url, opts) {
            if(typeof url === 'string' && url.startsWith('blob:')) {
              try {
                const r = await fetch(url);
                const txt = await r.text();
                const dataUrl = 'data:application/javascript;charset=utf-8,' + encodeURIComponent(txt);
                return _orig.call(this, dataUrl, opts);
              } catch(e) { /* fall through */ }
            }
            return _orig.call(this, url, opts);
          };
        })();
        """
    }

    private func loadWebUI() {
        // Rebuild the UserScript with the current (potentially refreshed) tokens
        // so the atDocumentStart injection is always fresh.
        let uc = webView.configuration.userContentController
        uc.removeAllUserScripts()
        uc.addUserScript(WKUserScript(
            source: buildAudioWorkletPatchScript(),
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        ))
        uc.addUserScript(WKUserScript(
            source: buildAuthJS(),
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        ))
        uc.addUserScript(WKUserScript(
            source: buildCompanionCSSScript(),
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        ))

        // Build SSR cookie from the current tokens.
        let access  = WMTokenStore.shared.accessToken
        let refresh = WMTokenStore.shared.refreshToken
        let exp     = Int(Date().timeIntervalSince1970) + 3600
        var userId  = "", email = ""
        let parts   = access.split(separator: ".")
        if parts.count >= 2 {
            var b64 = String(parts[1])
                .replacingOccurrences(of: "-", with: "+")
                .replacingOccurrences(of: "_", with: "/")
            let rem = b64.count % 4
            if rem > 0 { b64 += String(repeating: "=", count: 4 - rem) }
            if let data = Data(base64Encoded: b64),
               let p = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                userId = p["sub"]   as? String ?? ""
                email  = p["email"] as? String ?? ""
            }
        }
        let sessionObj: [String: Any] = [
            "access_token": access, "token_type": "bearer",
            "expires_in": 3600, "expires_at": exp,
            "refresh_token": refresh,
            "user": ["id": userId, "aud": "authenticated", "role": "authenticated",
                     "email": email, "email_confirmed_at": "2024-01-01T00:00:00Z",
                     "app_metadata": ["provider": "email", "providers": ["email"]],
                     "user_metadata": [:] as [String: Any],
                     "created_at": "2024-01-01T00:00:00Z",
                     "updated_at": "2024-01-01T00:00:00Z"] as [String: Any]
        ]
        let libraryURL = URL(string: "\(kBaseURL)/library")!
        guard let sessionData = try? JSONSerialization.data(withJSONObject: sessionObj),
              let sessionStr  = String(data: sessionData, encoding: .utf8) else {
            webView.load(URLRequest(url: libraryURL)); return
        }
        let props: [HTTPCookiePropertyKey: Any] = [
            .domain:  "water.95ent.ai",
            .path:    "/",
            .name:    "sb-hlqwvctfmxljjmosfcqq-auth-token",
            .value:   sessionStr,
            .secure:  "TRUE",
            .expires: Date().addingTimeInterval(3600)
        ]
        guard let cookie = HTTPCookie(properties: props) else {
            webView.load(URLRequest(url: libraryURL)); return
        }
        let store = webView.configuration.websiteDataStore.httpCookieStore
        store.setCookie(cookie) { [weak self] in
            guard let self else { return }
            // Force a read-back so WebKit's network layer flushes the store
            // before we fire the HTTP request (guards against a known timing race).
            store.getAllCookies { [weak self] _ in
                DispatchQueue.main.async {
                    self?.webView.load(URLRequest(url: libraryURL))
                }
            }
        }
    }

    // MARK: WKNavigationDelegate

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        webView.evaluateJavaScript(buildAuthJS(), completionHandler: nil)
        updateNavBar()

        guard let url = webView.url, url.host == "water.95ent.ai" else {
            injectCompanionUI(); return
        }

        let unauthCheck = """
        (function(){
          var txt = document.body && document.body.innerText || '';
          return txt.indexOf('Drop your tracks here') !== -1;
        })();
        """
        webView.evaluateJavaScript(unauthCheck) { [weak self] result, _ in
            guard let self else { return }
            let isUnauth = result as? Bool ?? false
            if isUnauth {
                if !self.authRetryDone {
                    // First time: reload with freshly injected token
                    self.authRetryDone = true
                    WMAPIClient.shared.refreshTokens { [weak self] ok in
                        guard let self else { return }
                        if ok {
                            self.loadWebUI()
                        } else {
                            // Token unrefreshable — send user to login
                            DispatchQueue.main.async {
                                (self.view.window?.windowController as? WMWindowController)?.showLogin()
                            }
                        }
                    }
                } else {
                    // Second time still unauth — token is dead, show login
                    (self.view.window?.windowController as? WMWindowController)?.showLogin()
                }
            } else {
                self.injectCompanionUI()
            }
        }
    }

    private var authRetryDone = false

    private func injectCompanionUI() {
        // Reveal the web content now that auth is confirmed — fade out the dark overlay.
        if let ov = loadingOverlay, !ov.isHidden {
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.2
                ov.animator().alphaValue = 0
            }, completionHandler: { ov.isHidden = true })
        }

        // CSS: hide desktop-only chrome; native nav bar (Swift) replaces web bottom nav
        let css = """
        /* ── HIDE DESKTOP + MOBILE CHROME ───────────────────── */
        #nav-bar { display: none !important; }
        [data-mobile-player] { display: none !important; }
        [data-bottom-nav] { display: none !important; }
        [data-genre-picker] { display: none !important; }
        [data-collection-header] { display: none !important; }
        [data-mobile-spacer] { display: none !important; }
        .fixed.inset-0.z-\\[200\\] { display: none !important; }
        .fixed.bottom-24, .fixed.bottom-20 { display: none !important; }
        h1.pl-3 { display: none !important; }
        .shrink-0[aria-hidden="true"] { display: none !important; }
        [data-pending-overlay] { display: none !important; }

        /* ── GLOBAL BASE ─────────────────────────────────────── */
        html, body, #__next { background: #0d0d0d !important; }
        [data-main-scroll=""] { padding: 0 !important; }

        /* ── GREETING — centered, one muted line ─────────────── */
        [data-greeting] {
          font-size: 11px !important; line-height: 1 !important;
          padding: 6px 12px 4px !important; font-weight: 400 !important;
          color: rgba(255,255,255,0.25) !important;
          white-space: nowrap !important; overflow: hidden !important;
          text-overflow: ellipsis !important; text-align: center !important;
          display: block !important;
        }

        /* ── TYPE TABS — centered ────────────────────────────── */
        [data-tabs-bar] { justify-content: center !important; }
        [data-tabs-bar] button { padding: 8px 10px !important; font-size: 12px !important; }

        /* ── HIDE BPM INLINE PANEL (use player bar drag instead) */
        [data-inline-bpm-panel] { display: none !important; }

        /* ── SEARCH BAR — compact ────────────────────────────── */
        [class*="rounded-full"][class*="border"][class*="bg-muted"] {
          min-height: 0 !important; height: 36px !important;
        }

        /* ── TABLE: hide column header row ──────────────────────*/
        thead tr { display: none !important; }

        /* ── TABLE ROWS — Splice density ─────────────────────── */
        tbody tr { height: 44px !important; min-height: 44px !important; }
        tbody td { padding-top: 0 !important; padding-bottom: 0 !important; }

        /* ── TRACK NAME ──────────────────────────────────────── */
        tbody tr span[class*="truncate"][class*="font-medium"] {
          font-size: 13px !important; font-weight: 500 !important;
        }
        """
        // JS: inject CSS + three-layer track detection for native drag
        // Layer 1: fetch interception — catches audio URL before it hits the network
        // Layer 2: <audio> element play events — reliable fallback
        // Layer 3: data-loop-id mouseover — activates once web app is deployed
        let js = """
        (function(){
          if(document.getElementById('__morphStyle')) return;
          var s=document.createElement('style');
          s.id='__morphStyle';
          s.textContent=`\(css)`;
          document.head.appendChild(s);

          var notify=function(id){
            if(id) window.webkit.messageHandlers.morphHover.postMessage(id);
          };

          // Layer 1: intercept fetch — catches both Supabase Storage URLs
          // (/demo/{id}.mp3) AND the API proxy (/api/melody/demo/{id}, no .mp3).
          if(!window.__morphFetchPatched){
            window.__morphFetchPatched=true;
            var _fetch=window.fetch;
            window.fetch=function(){
              var url=typeof arguments[0]==='string'?arguments[0]:(arguments[0]&&arguments[0].url)||'';
              var m=url.match(/\\/demo\\/([0-9a-f-]{36})/);
              if(m) notify(m[1]);
              return _fetch.apply(this,arguments);
            };
          }

          // Layer 2: watch <audio> play events — same flexible regex
          var patchAudio=function(){
            document.querySelectorAll('audio').forEach(function(a){
              if(a.__morphWatched) return;
              a.__morphWatched=true;
              a.addEventListener('play',function(){
                var m=a.src&&a.src.match(/\\/demo\\/([0-9a-f-]{36})/);
                if(m) notify(m[1]);
              });
            });
          };
          setInterval(patchAudio,800);
          patchAudio();

          // Dismiss any Sentry/React error toasts that surface due to hydration
          // mismatch (companion viewport vs SSR viewport). Auto-dismiss at 300 ms,
          // 800 ms, and 1.5 s to catch late-appearing toasts.
          var dismissErrToasts=function(){
            document.querySelectorAll('[data-sonner-toast]').forEach(function(el){
              if(el.textContent&&(el.textContent.indexOf('418')!==-1||el.textContent.indexOf('notified')!==-1)){
                var btn=el.querySelector('[data-close-button]')||el.querySelector('[aria-label*="lose"]');
                if(btn) btn.click(); else el.remove();
              }
            });
          };
          setTimeout(dismissErrToasts,300);
          setTimeout(dismissErrToasts,800);
          setTimeout(dismissErrToasts,1500);

          // Layer 3: data-loop-id mouseover (activates after web deploy of data-loop-id attr)
          document.addEventListener('mouseover',function(e){
            var el=e.target;
            for(var i=0;i<10&&el;i++,el=el.parentElement){
              if(el.dataset&&el.dataset.loopId){notify(el.dataset.loopId);return;}
            }
          },{passive:true});

          // Layer 4: mousedown on any row — fires before play, giving us the ID
          // the moment the user presses down (works alongside data-loop-id).
          document.addEventListener('mousedown',function(e){
            var el=e.target;
            for(var i=0;i<12&&el;i++,el=el.parentElement){
              if(el.dataset&&el.dataset.loopId){notify(el.dataset.loopId);return;}
            }
          },{passive:true});

          // Multi-select: Cmd+click adds track to selection set
          if(!window.__morphSelected){window.__morphSelected=new Set();}
          document.addEventListener('click',function(e){
            if(!e.metaKey&&!e.ctrlKey) return;
            var el=e.target;
            for(var i=0;i<12&&el;i++,el=el.parentElement){
              if(el.dataset&&el.dataset.loopId){
                var id=el.dataset.loopId;
                if(window.__morphSelected.has(id)){
                  window.__morphSelected.delete(id);
                  el.style.outline='';
                } else {
                  window.__morphSelected.add(id);
                  el.style.outline='2px solid rgba(26,144,160,0.8)';
                  el.style.outlineOffset='-2px';
                }
                window.webkit.messageHandlers.morphSelect.postMessage([...window.__morphSelected]);
                e.preventDefault();e.stopPropagation();
                return;
              }
            }
          },true);

          // MutationObserver: watch for the player bar to appear and force-show
          // the volume control (Tailwind hides it on mobile viewports).
          var showVol=function(){
            document.querySelectorAll('[class*="w-32"][class*="ml-1"]').forEach(function(el){
              if(el.className&&el.className.indexOf('hidden')!==-1){
                el.style.setProperty('display','flex','important');
                el.style.setProperty('min-width','72px','important');
                el.style.setProperty('width','72px','important');
              }
            });
          };
          showVol();
          var obs=new MutationObserver(showVol);
          obs.observe(document.body,{childList:true,subtree:true});

          // Hide companion artifacts that may render asynchronously
          var _hideArt=function(){
            // Genre picker overlay: fixed inset-0 z-[200]
            document.querySelectorAll('.fixed.inset-0').forEach(function(el){
              if(el.className.indexOf('z-[200]')!==-1){
                el.style.setProperty('display','none','important');
              }
            });
            // Mobile spacer: aria-hidden div with 72px height
            document.querySelectorAll('[aria-hidden="true"]').forEach(function(el){
              if(el.style.height&&el.style.height.indexOf('72px')!==-1){
                el.style.setProperty('display','none','important');
              }
            });
            // Loading overlay: absolute inset-0 z-10 dark background
            document.querySelectorAll('.absolute.inset-0').forEach(function(el){
              if(el.className.indexOf('z-10')!==-1&&el.className.indexOf('bg-background')!==-1){
                el.style.setProperty('display','none','important');
              }
            });
            // BPM·key sub-labels below track names (pre-deploy; replaced by tags after deploy)
            document.querySelectorAll('tbody td p').forEach(function(el){
              if(el.textContent.indexOf('bpm')!==-1){
                el.style.setProperty('display','none','important');
              }
            });
          };
          _hideArt();
          new MutationObserver(_hideArt).observe(document.body,{childList:true,subtree:true});
        })();
        """
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    @objc private func openFeedbackForm() {
        let js = """
        (function(){
          var btn=document.querySelector('button[aria-label="Send feedback"]');
          if(btn){btn.style.setProperty('display','flex','important');btn.click();return;}
          return 'NOT_FOUND';
        })();
        """
        webView.evaluateJavaScript(js) { [weak self] result, _ in
            if (result as? String) == "NOT_FOUND" {
                NSWorkspace.shared.open(URL(string: "mailto:hello@95ent.ai?subject=Morph%20Feedback")!)
            }
        }
    }

    // Called when plugin sends TRANSPORT — syncs web player to DAW transport.
    // No dedup guard: plugin sends position every 200ms while playing so the
    // web player can seek immediately when the user switches to a new loop.
    func injectTransport(playing: Bool, timeSecs: Double, ppq: Double = 0.0) {
        let js = """
        (function(){
          if(window.__waterCompanion&&window.__waterCompanion.transport){
            window.__waterCompanion.transport({playing:\(playing ? "true" : "false"),timeSecs:\(timeSecs),ppq:\(ppq)});
          }
        })();
        """
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    // MARK: WKScriptMessageHandler

    func userContentController(_ uc: WKUserContentController, didReceive message: WKScriptMessage) {
        if message.name == "morphHover", let id = message.body as? String {
            webView.hoveredId = id
            prefetchIfNeeded(id)
            preMorphIfNeeded(id)
        } else if message.name == "morphSelect", let ids = message.body as? [String] {
            selectedIds = Set(ids)
        } else if message.name == "morphCopy", let dict = message.body as? [String: String],
                  let base64 = dict["base64"], let filename = dict["filename"] {
            handleCopyModified(base64: base64, filename: filename)
        } else if message.name == "morphPlayerState", let dict = message.body as? [String: Any] {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                let morphed = dict["isMorphed"] as? Bool ?? false
                if self.isMorphed != morphed {
                    self.isMorphed = morphed
                    self.updateSyncUI()
                    // Notify JUCE plugin via state file (plugin polls /tmp/water-morph-isMorphed)
                    try? String(morphed ? "1" : "0").write(toFile: "/tmp/water-morph-isMorphed", atomically: true, encoding: .utf8)
                }
            }
        }
    }

    private func handleCopyModified(base64: String, filename: String) {
        guard let data = Data(base64Encoded: base64) else { return }
        // Write to a stable temp path (overwrite on each copy so /tmp stays clean).
        let url = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(filename)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            NSLog("[Morph] copyModified write failed: %@", error.localizedDescription)
            return
        }
        // Single NSPasteboardItem with two types:
        //   • fileURL   → Cmd+V in Logic imports the morphed WAV clip directly
        //   • string    → Cmd+V in any text field pastes the track name
        let nameText = (filename as NSString).deletingPathExtension
        let item = NSPasteboardItem()
        item.setData(url.dataRepresentation, forType: .fileURL)
        item.setString(nameText, forType: .string)
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.writeObjects([item])
    }

    // MARK: Selection bar

    private func updateSelectionBar() {
        let n = selectedIds.count
        selBar.isHidden = n == 0
        selBarH.constant = n == 0 ? 0 : 44
        selLabel.stringValue = n == 1 ? "1 track selected" : "\(n) tracks selected"
    }

    @objc private func onSelEditTapped() {
        let ids = Array(selectedIds)
        guard !ids.isEmpty else { return }
        let jsIds = ids.map { "\"\($0.replacingOccurrences(of: "\"", with: "\\\""))\"" }.joined(separator: ",")
        let js = "window.__waterCompanion && window.__waterCompanion.openBulkEdit([\(jsIds)]);"
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    private func handleMultiDrag(_ event: NSEvent) {
        let ids = Array(selectedIds)
        guard !ids.isEmpty else { return }
        let sync = WMSyncState.shared

        var items: [NSDraggingItem] = []
        for (i, id) in ids.enumerated() {
            let track = trackMap[id] ?? WMTrack(id: id, name: id, key: "", tags: "", bpm: 0, duration: 0, demoHash: "")
            prefetchIfNeeded(id)
            let label = sync.isConnected && track.bpm > 0
                ? WMMorphEngine.displayName(trackName: track.name, bpm: sync.bpm, key: sync.key)
                : "\(track.name).mp3"
            let semis    = sync.isConnected ? semitonesBetween(from: track.key, to: sync.key) : 0
            let pbItem   = NSPasteboardItem()
            if sync.isConnected && track.bpm > 0 {
                pbItem.setDataProvider(WMMorphDragProvider(track: track, targetBPM: sync.bpm,
                                                           semitoneDelta: semis, displayName: label),
                                       forTypes: [.fileURL])
            } else {
                pbItem.setDataProvider(WMDragFileProvider(track), forTypes: [.fileURL])
            }
            let dragItem = NSDraggingItem(pasteboardWriter: pbItem)
            let img: NSImage
            if #available(macOS 11.0, *) { img = NSWorkspace.shared.icon(for: .audio) }
            else { img = NSWorkspace.shared.icon(forFileType: "mp3") }
            img.size = NSSize(width: 32, height: 32)
            let offset = CGFloat(i) * 4
            let pt = selDragBtn.convert(event.locationInWindow, from: nil)
            dragItem.setDraggingFrame(NSRect(x: pt.x - 16 + offset, y: pt.y - 16 + offset, width: 32, height: 32),
                                      contents: img)
            items.append(dragItem)
        }
        selDragBtn.beginDraggingSession(with: items, event: event, source: selDragBtn)
    }

    // MARK: Track metadata (for drag)

    private func fetchTrackMap() {
        WMAPIClient.shared.fetchAllTracks { [weak self] _, tracks in
            guard let self else { return }
            self.trackMap = Dictionary(uniqueKeysWithValues: tracks.map { ($0.id, $0) })
            for t in tracks where FileManager.default.fileExists(atPath: t.cachedFile.path) {
                self.cachedIds.insert(t.id)
            }
        }
    }

    private func prefetchIfNeeded(_ id: String) {
        guard !cachedIds.contains(id), !prefetchingIds.contains(id),
              let track = trackMap[id] else { return }
        prefetchingIds.insert(id)
        WMAPIClient.shared.downloadForDrag(track) { [weak self] url in
            guard let self else { return }
            self.prefetchingIds.remove(id)
            if url != nil {
                self.cachedIds.insert(id)
                self.preMorphIfNeeded(id)  // start morph render once download completes
            }
        }
    }

    private func preMorphIfNeeded(_ id: String) {
        let sync = WMSyncState.shared
        guard sync.isConnected, let track = trackMap[id],
              track.bpm > 0,
              FileManager.default.fileExists(atPath: track.cachedFile.path),
              !morphingIds.contains(id) else { return }
        let tgt   = sync.bpm
        let semis = semitonesBetween(from: track.key, to: sync.key)
        let outName = WMMorphEngine.morphedName(trackName: track.name, bpm: tgt, semitones: semis)
        let outURL  = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(outName)
        guard !FileManager.default.fileExists(atPath: outURL.path) else { return }
        morphingIds.insert(id)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            _ = try? WMMorphEngine.render(input: track.cachedFile,
                                          sourceBPM: track.bpm,
                                          targetBPM: tgt,
                                          semitoneDelta: semis,
                                          trackName: track.name)
            DispatchQueue.main.async { self?.morphingIds.remove(id) }
        }
    }

    // MARK: Native drag to DAW

    private func handleDrag(_ id: String, _ event: NSEvent) {
        let track: WMTrack = trackMap[id] ??
            WMTrack(id: id, name: id, key: "", tags: "", bpm: 0, duration: 0, demoHash: "")

        // Only drag when file is already downloaded — trigger prefetch for next time if not.
        guard FileManager.default.fileExists(atPath: track.cachedFile.path) else {
            prefetchIfNeeded(id)
            return
        }
        prefetchIfNeeded(id)  // no-op if already cached

        let sync = WMSyncState.shared
        let canMorph = sync.isConnected
            && track.bpm > 0
            && FileManager.default.fileExists(atPath: track.cachedFile.path)

        func startDragWith(fileURL: URL, named: String) {
            let pbItem = NSPasteboardItem()
            let provider = WMNamedDragProvider(source: fileURL, displayName: named)
            pbItem.setDataProvider(provider, forTypes: [.fileURL])
            let dragItem = NSDraggingItem(pasteboardWriter: pbItem)
            let img: NSImage
            if #available(macOS 11.0, *) { img = NSWorkspace.shared.icon(for: .audio) }
            else { img = NSWorkspace.shared.icon(forFileType: "mp3") }
            img.size = NSSize(width: 32, height: 32)
            let pt = webView.convert(event.locationInWindow, from: nil)
            dragItem.setDraggingFrame(NSRect(x: pt.x - 16, y: pt.y - 16, width: 32, height: 32),
                                      contents: img)
            webView.beginDraggingSession(with: [dragItem], event: event, source: webView)
            webView.hoveredId = ""
        }

        if canMorph {
            let tgtBPM = sync.bpm
            let semis  = semitonesBetween(from: track.key, to: sync.key)
            let label  = WMMorphEngine.displayName(trackName: track.name, bpm: tgtBPM, key: sync.key)

            // Start drag immediately with a provider that morphs on demand
            let pbItem = NSPasteboardItem()
            let provider = WMMorphDragProvider(track: track, targetBPM: tgtBPM, semitoneDelta: semis, displayName: label)
            pbItem.setDataProvider(provider, forTypes: [.fileURL])
            let dragItem = NSDraggingItem(pasteboardWriter: pbItem)
            let img: NSImage
            if #available(macOS 11.0, *) { img = NSWorkspace.shared.icon(for: .audio) }
            else { img = NSWorkspace.shared.icon(forFileType: "mp3") }
            img.size = NSSize(width: 32, height: 32)
            let pt = webView.convert(event.locationInWindow, from: nil)
            dragItem.setDraggingFrame(NSRect(x: pt.x - 16, y: pt.y - 16, width: 32, height: 32),
                                      contents: img)
            webView.beginDraggingSession(with: [dragItem], event: event, source: webView)
            webView.hoveredId = ""
        } else {
            // No sync or file not cached yet — lazy raw drag
            let pbItem = NSPasteboardItem()
            pbItem.setDataProvider(WMDragFileProvider(track), forTypes: [.fileURL])
            let dragItem = NSDraggingItem(pasteboardWriter: pbItem)
            let img: NSImage
            if #available(macOS 11.0, *) { img = NSWorkspace.shared.icon(for: .audio) }
            else { img = NSWorkspace.shared.icon(forFileType: "mp3") }
            img.size = NSSize(width: 32, height: 32)
            let pt = webView.convert(event.locationInWindow, from: nil)
            dragItem.setDraggingFrame(NSRect(x: pt.x - 16, y: pt.y - 16, width: 32, height: 32),
                                      contents: img)
            webView.beginDraggingSession(with: [dragItem], event: event, source: webView)
            webView.hoveredId = ""
        }
    }
}

// MARK: - Window controller

final class WMWindowController: NSWindowController {

    convenience init() {
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 380, height: 720),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        panel.title                       = "Water"
        panel.level                       = .floating
        panel.hidesOnDeactivate           = false
        panel.isMovableByWindowBackground = true
        panel.backgroundColor             = kBG
        // Fixed width — vertical-only resize like Splice
        panel.contentMinSize = NSSize(width: 380, height: 400)
        panel.contentMaxSize = NSSize(width: 380, height: 9999)
        panel.collectionBehavior = [.fullScreenAuxiliary, .moveToActiveSpace]
        panel.isRestorable = false  // don't restore saved (possibly huge) window size
        self.init(window: panel)

        // Always open full height of the visible screen, right-aligned.
        // User can resize; their preferred height is saved and restored next launch.
        let savedKey = "WaterMorphPanelFrame"
        if let screen = NSScreen.main {
            let f = screen.visibleFrame
            var h = f.height
            var x = f.maxX - 400
            var y = f.minY
            // If user previously resized, honour their height + position.
            if let saved = UserDefaults.standard.string(forKey: savedKey) {
                let r = NSRectFromString(saved)
                h = max(400, min(r.size.height, f.height))
                x = min(max(r.origin.x, f.minX), f.maxX - 380)
                y = min(max(r.origin.y, f.minY), f.maxY - h)
            }
            panel.setFrame(NSRect(x: x, y: y, width: 380, height: h), display: false)
        } else {
            panel.setContentSize(NSSize(width: 380, height: 720))
        }

        // Persist frame on every move/resize
        NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: panel, queue: .main) { [weak panel] _ in
            if let f = panel?.frame { UserDefaults.standard.set(NSStringFromRect(f), forKey: savedKey) }
        }
        NotificationCenter.default.addObserver(forName: NSWindow.didResizeNotification, object: panel, queue: .main) { [weak panel] _ in
            if let f = panel?.frame { UserDefaults.standard.set(NSStringFromRect(f), forKey: savedKey) }
        }
    }

    func start() {
        UserDefaults.standard.removeObject(forKey: "WaterMorphPanelFrame")
        WMTokenStore.shared.load()
        WMTokenStore.shared.isLoggedIn ? showLibrary() : showLogin()
        // Force full-height frame on every launch — ignore any stale saved size.
        if let panel = window, let screen = NSScreen.main {
            let f = screen.visibleFrame
            panel.setFrame(NSRect(x: f.maxX - 400, y: f.minY, width: 380, height: f.height), display: false)
        }
        showWindow(nil)
        snapToFullSize()
    }

    // Clamp position to screen and enforce 380px width. Height is user-controlled.
    func snapToFullSize() {
        guard let panel = window, let screen = NSScreen.main else { return }
        let f = screen.visibleFrame
        var frame = panel.frame
        frame.size.width = 380
        frame.origin.x = min(max(frame.origin.x, f.minX), f.maxX - 380)
        frame.origin.y = min(max(frame.origin.y, f.minY), f.maxY - frame.size.height)
        panel.setFrame(frame, display: true)
    }

    var webLibraryVC: WMWebLibraryVC? {
        window?.contentViewController as? WMWebLibraryVC
    }

    func showLibrary() {
        window?.contentViewController = WMWebLibraryVC()
        window?.title = "Water"
    }

    func showLogin() {
        window?.contentViewController = WMLoginVC()
        window?.title = "Morph — Sign In"
    }
}

// MARK: - Menu bar item

// Draws the Water "W" wave logo as a 18×12 template NSImage using NSBezierPath.
// Template images are automatically rendered white/black by AppKit depending on
// menu bar appearance — no separate dark/light assets needed.
private func makeWaterMenuBarIcon() -> NSImage {
    let w: CGFloat = 18, h: CGFloat = 12
    let img = NSImage(size: NSSize(width: w, height: h))
    img.isTemplate = true
    img.lockFocus()
    NSColor.black.setFill()
    // Three-hump wave approximating the Water logo "W" shape.
    // Coordinates normalised to 18×12 pt viewport.
    let path = NSBezierPath()
    path.move(to: NSPoint(x: 0, y: 6))
    // Left hump (down)
    path.curve(to: NSPoint(x: 4.5, y: 1),
               controlPoint1: NSPoint(x: 1.2, y: 6),
               controlPoint2: NSPoint(x: 1.5, y: 1))
    // Back up to centre-left
    path.curve(to: NSPoint(x: 9, y: 6),
               controlPoint1: NSPoint(x: 7.5, y: 1),
               controlPoint2: NSPoint(x: 7.8, y: 6))
    // Right hump (down)
    path.curve(to: NSPoint(x: 13.5, y: 1),
               controlPoint1: NSPoint(x: 10.2, y: 6),
               controlPoint2: NSPoint(x: 10.5, y: 1))
    // Back up to right edge
    path.curve(to: NSPoint(x: 18, y: 6),
               controlPoint1: NSPoint(x: 16.5, y: 1),
               controlPoint2: NSPoint(x: 16.8, y: 6))
    path.lineWidth = 2.0
    path.lineCapStyle = .round
    path.lineJoinStyle = .round
    NSColor.black.setStroke()
    path.stroke()
    img.unlockFocus()
    return img
}

final class WMMenuBar: NSObject {
    private var item: NSStatusItem?
    private let wc: WMWindowController

    init(wc: WMWindowController) { self.wc = wc; super.init() }

    func install() {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let img = makeWaterMenuBarIcon()
        item?.button?.image  = img
        item?.button?.imagePosition = .imageLeft
        item?.button?.title  = ""
        item?.button?.target = self
        item?.button?.action = #selector(toggle)
    }

    @objc private func toggle() {
        guard let w = wc.window else { return }
        if w.isVisible { w.orderOut(nil) } else {
            wc.showWindow(nil)
            wc.snapToFullSize()
        }
    }
}

// MARK: - TCP server (IPC with plugin — localhost:59812)

private func runSocketServer() {
    let serverFd = socket(AF_INET, SOCK_STREAM, 0)
    guard serverFd >= 0 else { return }

    var reuse: Int32 = 1
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))

    var addr            = sockaddr_in()
    addr.sin_family     = sa_family_t(AF_INET)
    addr.sin_port       = kTCPPort.bigEndian
    addr.sin_addr       = in_addr(s_addr: inet_addr("127.0.0.1"))

    let addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)
    withUnsafePointer(to: &addr) { ptr in
        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
            _ = Darwin.bind(serverFd, sa, addrLen)
        }
    }
    listen(serverFd, 8)
    NSLog("[Morph] TCP listening on 127.0.0.1:%d", kTCPPort)
    while true {
        let fd = accept(serverFd, nil, nil)
        guard fd >= 0 else { continue }
        Thread.detachNewThread { handleClient(fd) }
    }
}

private func handleClient(_ fd: Int32) {
    _ = write(fd, "READY\n", 6)
    var buf  = [UInt8](repeating: 0, count: 2048)
    var pending = ""
    while true {
        let n = read(fd, &buf, buf.count - 1)
        guard n > 0 else { break }
        pending += String(bytes: buf.prefix(Int(n)), encoding: .utf8) ?? ""
        while let nl = pending.firstIndex(of: "\n") {
            let line = String(pending[..<nl]).trimmingCharacters(in: .whitespaces)
            pending.removeSubrange(pending.startIndex...nl)
            if line == "PING" { _ = write(fd, "PONG\n", 5) }
            else if line.hasPrefix("DRAG ") { _ = write(fd, "OK\n", 3) }
            else if line.hasPrefix("SYNC ") {
                let parts    = line.dropFirst(5).split(separator: " ").map(String.init)
                let bpm      = Double(parts.first ?? "") ?? 0
                let rawKey   = parts.count > 1 ? parts[1] : ""
                let key      = (rawKey == "?" || rawKey.isEmpty) ? "" : rawKey
                let mode     = parts.count > 2 ? parts[2] : "project"
                let timeSecs = parts.count > 3 ? (Double(parts[3]) ?? 0.0) : 0.0
                DispatchQueue.main.async {
                    WMSyncState.shared.update(bpm: bpm, key: key, mode: mode, timeSecs: timeSecs)
                }
            } else if line.hasPrefix("TRANSPORT ") {
                let parts    = line.dropFirst(10).split(separator: " ").map(String.init)
                let playing  = (parts.first ?? "").hasPrefix("play")
                let timeSecs = parts.count > 1 ? (Double(parts[1]) ?? 0.0) : 0.0
                let ppq      = parts.count > 2 ? (Double(parts[2]) ?? 0.0) : 0.0
                DispatchQueue.main.async {
                    if let appDelegate = NSApp.delegate as? WMAppDelegate {
                        appDelegate.wc.webLibraryVC?.injectTransport(playing: playing, timeSecs: timeSecs, ppq: ppq)
                    }
                }
            }
        }
    }
    close(fd)
}

// MARK: - App delegate & entry point

final class WMAppDelegate: NSObject, NSApplicationDelegate {
    var wc:  WMWindowController!
    var bar: WMMenuBar!

    func applicationWillFinishLaunching(_ notification: Notification) {
        NSAppleEventManager.shared().setEventHandler(
            self,
            andSelector: #selector(handleGetURL(_:withReplyEvent:)),
            forEventClass: AEEventClass(kInternetEventClass),
            andEventID: AEEventID(kAEGetURL)
        )
    }

    @objc func handleGetURL(_ event: NSAppleEventDescriptor, withReplyEvent: NSAppleEventDescriptor) {
        // watermorph://launch — snap to full size, surface and focus
        DispatchQueue.main.async { [weak self] in
            self?.wc?.showWindow(nil)
            self?.wc?.snapToFullSize()   // after show so frame sticks
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Single-instance guard — if already running, just surface the window and exit.
        let running = NSRunningApplication.runningApplications(withBundleIdentifier: "ai.95ent.watermorph.helper")
        if running.count > 1 {
            NSApp.terminate(nil)
            return
        }
        NSApp.setActivationPolicy(.accessory)
        wc  = WMWindowController()
        bar = WMMenuBar(wc: wc)
        bar.install()
        wc.start()
        Thread.detachNewThread { runSocketServer() }
        startLogicAudioWatcher()
    }

    // Watches which audio files Logic Pro has open every 2s.
    // Writes new file path to /tmp/water-morph-analyze-file.txt → plugin reads it.
    private var logicWatchTimer: Timer?
    private var lastAnalyzedAudioPath = ""

    private func startLogicAudioWatcher() {
        logicWatchTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.checkLogicAudioFile()
        }
    }

    private func checkLogicAudioFile() {
        DispatchQueue.global(qos: .background).async { [weak self] in
            guard let self else { return }
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/bin/sh")
            task.arguments = ["-c",
                "LPID=$(pgrep -x 'Logic Pro' | head -1); " +
                "[ -z \"$LPID\" ] && exit 0; " +
                "lsof -F n -p \"$LPID\" 2>/dev/null | sed -n 's/^n//p' | " +
                "grep -iE '\\.(wav|aiff|aif|mp3|m4a)$' | " +
                "grep -v '/System\\|/Library/Audio\\|/Applications\\|\\.component' | " +
                "head -1"]
            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError  = Pipe()
            guard (try? task.run()) != nil else { return }
            task.waitUntilExit()
            let path = String(data: pipe.fileHandleForReading.readDataToEndOfFile(),
                              encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !path.isEmpty, path != self.lastAnalyzedAudioPath else { return }
            self.lastAnalyzedAudioPath = path
            try? path.write(toFile: "/tmp/water-morph-analyze-file.txt",
                            atomically: true, encoding: .utf8)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { false }
}

let app      = NSApplication.shared
let delegate = WMAppDelegate()
app.delegate = delegate
app.run()
