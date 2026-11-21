const WIDTH = 160;
const HEIGHT = 96;
const TILE = 16;
const TILE_COLS = WIDTH / TILE;
const TILE_COUNT = TILE_COLS * HEIGHT / TILE;

const source = document.querySelector("#source");
const sourceContext = source.getContext("2d", { willReadFrequently: true });
const upload = document.querySelector("#upload");
const sampleButton = document.querySelector("#sample");
const runButton = document.querySelector("#run");
const port = document.querySelector("#port");
const refresh = document.querySelector("#refresh");
const connection = document.querySelector("#connection");
const runStatus = document.querySelector("#run-status");
const result = document.querySelector("#result");
const number = new Intl.NumberFormat("vi-VN");
const temperature = new Intl.NumberFormat("vi-VN", { minimumFractionDigits: 1, maximumFractionDigits: 1 });
const duration = new Intl.NumberFormat("vi-VN", { maximumFractionDigits: 1 });
const seconds = new Intl.NumberFormat("vi-VN", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const template = document.querySelector("#tick-template");
const cases = new Map();

let image = null;
let loadingImage = false;
let busy = false;

for (const card of document.querySelectorAll("[data-mode]")) {
  cases.set(card.dataset.mode, {
    output: card.querySelector(".output"),
    canvas: card.querySelector("canvas").getContext("2d"),
    fields: Object.fromEntries([...card.querySelectorAll("[data-field]")].map(node => [node.dataset.field, node])),
    lane: card.querySelector(".timeline ol"),
    received: 0,
    sent: 0,
    samples: 0,
    peakBorrowed: 0,
    frameDone: false,
  });
}

function updateButtons() {
  runButton.disabled = busy || loadingImage || !image || !port.value;
  port.disabled = busy || !port.value;
  refresh.disabled = busy;
  upload.disabled = sampleButton.disabled = busy || loadingImage;
  document.querySelector('label[for="upload"]').setAttribute("aria-disabled", String(upload.disabled));
}

function status(message, kind = "") {
  runStatus.textContent = message;
  runStatus.dataset.kind = kind;
}

function resetResults() {
  result.hidden = true;
  for (const view of cases.values()) {
    view.canvas.clearRect(0, 0, WIDTH, HEIGHT);
    view.output.classList.remove("has-output");
    view.fields.state.textContent = "Chờ chạy";
    delete view.fields.state.dataset.rank;
    view.fields.tiles.textContent = view.fields.samples.textContent = view.fields.time.textContent = "—";
    view.fields.progress.value = 0;
    view.fields.memory.textContent = "Từng ô ảnh sẽ hiện khi bo gửi về.";
    view.fields.batches.textContent = view.fields.core.textContent = "";
    view.received = view.sent = view.samples = view.peakBorrowed = 0;
    view.frameDone = false;
    view.lane.replaceChildren();
  }
}

function setImage(name) {
  const pixels = sourceContext.getImageData(0, 0, WIDTH, HEIGHT);
  image = new Uint8Array(WIDTH * HEIGHT);
  for (let i = 0; i < image.length; i++) {
    const offset = 4 * i;
    const gray = Math.round(.299 * pixels.data[offset] + .587 * pixels.data[offset + 1] + .114 * pixels.data[offset + 2]);
    image[i] = gray;
    pixels.data[offset] = pixels.data[offset + 1] = pixels.data[offset + 2] = gray;
  }
  sourceContext.putImageData(pixels, 0, 0);
  document.querySelector("#image-name").textContent = name + " · " + WIDTH + " × " + HEIGHT;
  resetResults();
}

async function loadImage(blob, name) {
  const bitmap = await createImageBitmap(blob);
  try {
    const scale = Math.max(WIDTH / bitmap.width, HEIGHT / bitmap.height);
    const width = bitmap.width * scale;
    const height = bitmap.height * scale;
    sourceContext.fillStyle = "#ffffff";
    sourceContext.fillRect(0, 0, WIDTH, HEIGHT);
    sourceContext.drawImage(bitmap, (WIDTH - width) / 2, (HEIGHT - height) / 2, width, height);
    setImage(name);
  } finally {
    bitmap.close();
  }
}

async function useSample() {
  loadingImage = true;
  updateButtons();
  try {
    const response = await fetch("/sample.jpg");
    if (!response.ok) throw new Error("Sample unavailable");
    await loadImage(await response.blob(), "Ảnh mẫu ESP-IDF");
  } catch {
    sourceContext.fillStyle = "#ddd";
    sourceContext.fillRect(0, 0, WIDTH, HEIGHT);
    sourceContext.fillStyle = "#505050";
    for (let x = 0; x < WIDTH; x += 16) sourceContext.fillRect(x, 0, 4, HEIGHT);
    for (let y = 0; y < HEIGHT; y += 16) sourceContext.fillRect(0, y, WIDTH, 4);
    sourceContext.fillStyle = "#242424";
    sourceContext.fillRect(18, 25, 124, 46);
    sourceContext.fillStyle = "#f4f4f4";
    sourceContext.font = "bold 32px system-ui";
    sourceContext.fillText("ESP32", 28, 59, 104);
    setImage("Bảng kiểm tra độ nét");
  } finally {
    loadingImage = false;
    updateButtons();
  }
}

async function scanPorts() {
  const selected = port.value;
  refresh.disabled = true;
  try {
    const response = await fetch("/api/ports");
    if (!response.ok) throw new Error("Port scan failed");
    const { ports, preferred } = await response.json();
    port.replaceChildren(...ports.map(({ device, label }) => {
      const option = new Option(device, device);
      option.title = label;
      return option;
    }));
    if (!ports.length) {
      port.add(new Option("Chưa có cổng", ""));
      connection.textContent = "Chưa phát hiện cổng USB–UART";
      status("Cắm bo qua cổng UART, rồi bấm Quét lại.");
    } else {
      if (ports.some(item => item.device === selected)) port.value = selected;
      else if (ports.some(item => item.device === preferred)) port.value = preferred;
      connection.textContent = "Cổng " + port.value;
      status("Sẵn sàng chạy hai chính sách cùng lúc.");
    }
    delete connection.dataset.active;
  } catch {
    port.replaceChildren(new Option("Chưa có cổng", ""));
    status("Không đọc được danh sách cổng từ chương trình chạy cục bộ.", "error");
  } finally {
    updateButtons();
  }
}

function drawTile(view, { index, gray }) {
  const pixels = view.canvas.createImageData(TILE, TILE);
  for (let i = 0; i < TILE * TILE; i++) {
    const value = Number.parseInt(gray.slice(i * 2, i * 2 + 2), 16);
    const output = 4 * i;
    pixels.data[output] = pixels.data[output + 1] = pixels.data[output + 2] = value;
    pixels.data[output + 3] = 255;
  }
  view.canvas.putImageData(pixels, (index % TILE_COLS) * TILE, Math.floor(index / TILE_COLS) * TILE);
  view.output.classList.add("has-output");
  view.received++;
  view.fields.tiles.textContent = view.received + " / " + TILE_COUNT;
  view.fields.progress.value = view.received;
}

function renderSample(view, event) {
  const item = template.content.firstElementChild.cloneNode(true);
  item.querySelector(".tick-caption").textContent = event.phase === "before" ? "Trước"
    : event.phase === "after" ? "Sau" : "Mẫu " + event.index;
  const ok = event.status === "OK";
  const description = {
    NO_MEMORY: "Thiếu bộ đệm",
    QUOTA: "Vượt hạn mức",
    SENSOR_ERROR: "Lỗi cảm biến",
  }[event.status];
  item.dataset.status = ok ? "OK" : "lost";
  item.querySelector("strong").textContent = ok ? temperature.format(event.temperature_mc / 1000) + " °C" : "Mất gói";
  item.querySelector("small").textContent = ok ? "+" + number.format(event.time_ms) + " ms" : description;
  item.title = "+" + event.time_ms + " ms" + (ok ? "" : " · " + description);
  view.lane.append(item);
  view.lane.scrollLeft = view.lane.scrollWidth;
  if (event.phase === "during") {
    view.samples++;
    if (ok) view.sent++;
    view.fields.samples.textContent = view.sent + " / " + view.samples;
  }
}

function renderEvent(event) {
  if (event.event === "error") throw new Error(event.message);
  if (event.event === "start") {
    connection.textContent = "ESP32-S3 · " + port.value;
    connection.dataset.active = "true";
    for (const view of cases.values()) {
      view.fields.state.textContent = "Đang lọc ảnh";
      view.fields.tiles.textContent = "0 / " + TILE_COUNT;
      view.fields.samples.textContent = "0 / 0";
    }
    status("Đang chạy đồng thời trên hai lõi. Cả hai gửi kết quả qua cùng cổng UART.");
    return false;
  }
  if (event.event === "complete") {
    const shared = cases.get("SHARED");
    const adaptive = cases.get("ADAPTIVE");
    let finish = "Cả hai nhận đủ ảnh cùng thời điểm.";
    if (event.first_mode) {
      for (const [mode, view] of cases) {
        const first = mode === event.first_mode;
        view.fields.state.textContent = first ? "Hoàn tất đầu tiên" : "Hoàn tất thứ hai";
        if (first) view.fields.state.dataset.rank = "first";
      }
      const delta = event.finish_gap_us < 1000 ? number.format(event.finish_gap_us) + " µs"
        : duration.format(event.finish_gap_us / 1000) + " ms";
      finish = (event.first_mode === "SHARED" ? "Heap chung" : "Adaptive Heap") + " gửi đủ ảnh trước " + delta + ".";
    }
    document.querySelector("#result-text").textContent =
      "Heap chung gửi được " + shared.sent + "/" + shared.samples + " bản tin nhiệt độ trong lượt. "
      + "Adaptive Heap gửi được " + adaptive.sent + "/" + adaptive.samples + ".";
    document.querySelector("#integrity").textContent =
      "Hai lõi bắt đầu cách nhau " + number.format(event.start_skew_us) + " µs. Cả "
      + event.matching_tiles + " ô ảnh đầu ra trùng từng pixel.";
    result.hidden = false;
    connection.textContent = "ESP32-S3 · " + port.value;
    status(finish, "complete");
    return true;
  }
  const view = cases.get(event.mode);
  switch (event.event) {
    case "lane_start":
      view.fields.core.textContent = "Lõi " + event.core + " · bắt đầu +" + number.format(event.offset_us) + " µs";
      break;
    case "batch":
      view.peakBorrowed = Math.max(view.peakBorrowed, event.borrowed);
      view.fields.memory.textContent = "Lô " + event.batch + " · " + event.count + " ô · A giữ "
        + number.format(event.charged) + " B" + (event.borrowed ? ", mượn " + number.format(event.borrowed) + " B." : ".");
      view.fields.state.textContent = "Đang gửi lô " + event.batch;
      break;
    case "sample":
      renderSample(view, event);
      break;
    case "tile":
      drawTile(view, event);
      break;
    case "released":
      view.fields.memory.textContent = "Đã giải phóng buffer lô " + event.batch + ".";
      view.fields.state.textContent = view.received < TILE_COUNT ? "Đang lọc lô tiếp" : "Đã nhận đủ ảnh";
      break;
    case "done":
      view.frameDone = true;
      view.fields.state.textContent = "Hoàn tất";
      view.fields.time.textContent = seconds.format(event.frame_us / 1000000) + " s";
      view.fields.batches.textContent = event.batches + " lô · Lọc + cấp phát " + duration.format(event.filter_us / 1000) + " ms";
      view.fields.memory.textContent = "A đã trả hết buffer."
        + (view.peakBorrowed ? " Mượn tối đa " + number.format(view.peakBorrowed) + " B." : "");
      break;
  }
  return false;
}

async function runComparison() {
  if (runButton.disabled) return;
  busy = true;
  updateButtons();
  resetResults();
  status("Đang mở " + port.value + " và gửi ảnh…");
  let reader;
  try {
    const response = await fetch("/api/compare?port=" + encodeURIComponent(port.value), {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: image,
    });
    if (response.status === 409) throw new Error("Một lượt so sánh khác đang chạy.");
    if (response.status === 400) throw new Error("Cổng UART hoặc dữ liệu ảnh không hợp lệ. Hãy quét cổng và chọn lại ảnh.");
    if (!response.ok || !response.body) throw new Error("Không kết nối được chương trình điều khiển UART.");
    reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
    let pending = "";
    let complete = false;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      pending += value;
      let newline;
      while ((newline = pending.indexOf("\n")) !== -1) {
        complete = renderEvent(JSON.parse(pending.slice(0, newline))) || complete;
        pending = pending.slice(newline + 1);
      }
    }
    if (!complete) throw new Error("Luồng UART kết thúc trước khi có đủ hai lượt chạy.");
  } catch (error) {
    status(error.message, "error");
    for (const view of cases.values()) {
      if (!view.frameDone) view.fields.state.textContent = "Chưa hoàn tất";
    }
    delete connection.dataset.active;
  } finally {
    if (reader) await reader.cancel().catch(() => {});
    busy = false;
    updateButtons();
  }
}

refresh.addEventListener("click", scanPorts);
port.addEventListener("change", () => {
  connection.textContent = "Cổng " + port.value;
  delete connection.dataset.active;
  updateButtons();
});
sampleButton.addEventListener("click", useSample);
upload.addEventListener("change", async () => {
  const file = upload.files[0];
  if (!file) return;
  loadingImage = true;
  updateButtons();
  try {
    await loadImage(file, file.name);
  } catch {
    status("Không mở được ảnh. Hãy chọn tệp JPEG hoặc PNG.", "error");
  } finally {
    upload.value = "";
    loadingImage = false;
    updateButtons();
  }
});
runButton.addEventListener("click", runComparison);
useSample();
scanPorts();
