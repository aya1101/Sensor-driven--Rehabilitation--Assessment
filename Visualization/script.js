// Global variables
let charts = {};
let sensorData = [];
let sampleRate = 100; // Default sample rate, will be calculated

// DOM Elements
const fileUpload = document.getElementById('file-upload');
const tabsContainer = document.getElementById('tabs');
const initialMessage = document.getElementById('initial-message');
const loadingIndicator = document.getElementById('loading');
const chartArea = document.getElementById('chart-area');
const resetZoomBtn = document.getElementById('reset-zoom-btn');

// DFT Controls
const dftSignalSelector = document.getElementById('dft-signal-selector');

// Smoothing Controls
const maWindowSize = document.getElementById('ma-window-size');
const maWindowValue = document.getElementById('ma-window-value');

// Segmentation Controls
const segmentationSignalSelector = document.getElementById('segmentation-signal-selector');
const peakThresholdInput = document.getElementById('peak-threshold');
const peakDistanceInput = document.getElementById('peak-distance');
const applySegmentationBtn = document.getElementById('apply-segmentation-btn');
const peakCountSpan = document.getElementById('peak-count');


// Event listeners
fileUpload.addEventListener('change', handleFileUpload);
resetZoomBtn.addEventListener('click', resetVisibleChartsZoom);
dftSignalSelector.addEventListener('change', handleDftSignalChange);
maWindowSize.addEventListener('input', () => {
    maWindowValue.textContent = maWindowSize.value;
    renderSmoothedCharts(); // Update charts in real-time
});
// Apply segmentation when button is clicked OR when the signal selector changes
applySegmentationBtn.addEventListener('click', renderSegmentationChart);
segmentationSignalSelector.addEventListener('change', renderSegmentationChart);


/**
 * Main function to handle file upload and processing.
 */
async function handleFileUpload(event) {
    const file = event.target.files[0];
    if (!file) return;

    initialMessage.classList.add('hidden');
    loadingIndicator.classList.remove('hidden');
    chartArea.classList.add('hidden');
    
    destroyAllCharts();
    // Clear previous chart containers
    document.getElementById('raw').innerHTML = '';
    document.querySelector('#smoothed .grid').innerHTML = '';
    document.getElementById('segmentation').querySelector('.chart-container').innerHTML = '';
    document.querySelector('#dft .chart-container').innerHTML = '';

    try {
        const csvText = await file.text();
        sensorData = parseCSV(csvText);
        calculateMagnitudes();
        calculateSampleRate();
        
        processAndRenderAllCharts();

        tabsContainer.classList.remove('hidden');
        chartArea.classList.remove('hidden');
        changeTab('raw'); 
    } catch (error) {
        console.error("Lỗi xử lý tệp:", error);
        alert("Đã xảy ra lỗi khi đọc hoặc xử lý tệp CSV. Vui lòng kiểm tra định dạng tệp.");
        initialMessage.classList.remove('hidden');
    } finally {
        loadingIndicator.classList.add('hidden');
    }
}

/**
 * Handles changes from the DFT signal selector.
 */
function handleDftSignalChange() {
    if (sensorData.length === 0) return;
    renderDftChart();
}


/**
 * Resets the zoom level on all charts within the currently visible tab.
 */
function resetVisibleChartsZoom() {
    const activeTab = document.querySelector('.tab-content:not(.hidden)');
    if (!activeTab) return;
    const canvasElements = activeTab.querySelectorAll('canvas');
    canvasElements.forEach(canvas => {
        const chartId = canvas.id;
        if (charts[chartId] && charts[chartId].resetZoom) {
            charts[chartId].resetZoom();
        }
    });
}


// --- DATA PROCESSING AND CALCULATION ---

function parseCSV(text) {
    const lines = text.trim().split('\n');
    if (lines.length < 2) return [];
    const headers = lines[0].split(',').map(h => h.trim());
    return lines.slice(1).map(line => {
        const values = line.split(',');
        return headers.reduce((obj, header, index) => {
            const value = parseFloat(values[index]);
            obj[header] = isNaN(value) ? (values[index] ? values[index].trim() : '') : value;
            return obj;
        }, {});
    });
}

function calculateMagnitudes() {
    sensorData.forEach(d => {
        d.AccMagnitude = Math.sqrt(d.AccX**2 + d.AccY**2 + d.AccZ**2);
        d.GyroMagnitude = Math.sqrt(d.GyroX**2 + d.GyroY**2 + d.GyroZ**2);
    });
}

function calculateSampleRate() {
    if (sensorData.length < 2) {
        sampleRate = 100; // Default
        return;
    }
    const timeDiff = (sensorData[sensorData.length - 1].Timestamp_us - sensorData[0].Timestamp_us) / 1e6;
    if (timeDiff === 0) {
        sampleRate = 100;
        console.warn("Time difference is zero, using default sample rate.");
        return;
    }
    sampleRate = (sensorData.length - 1) / timeDiff;
}

// --- PEAK FINDING & SMOOTHING LOGIC ---

function movingAverage(data, windowSize) {
    if (windowSize <= 1) return data;
    const result = [];
    for (let i = 0; i < data.length; i++) {
        const start = Math.max(0, i - Math.floor(windowSize / 2));
        const end = Math.min(data.length, i + Math.ceil(windowSize / 2));
        let sum = 0;
        for (let j = start; j < end; j++) sum += data[j];
        result.push(sum / (end - start));
    }
    return result;
}

/**
 * Finds peaks in a 1D array.
 * @param {number[]} data - The input data array.
 * @param {object} options - Options for peak finding.
 * @param {number} options.threshold - The minimum value for a peak.
 * @param {number} options.distance - The minimum distance between peaks.
 * @returns {number[]} An array of indices of the found peaks.
 */
function findPeaks(data, { threshold, distance }) {
    const peaks = [];
    // Find all points above threshold that are local maxima
    for (let i = 1; i < data.length - 1; i++) {
        if (data[i] > threshold && data[i] > data[i - 1] && data[i] > data[i + 1]) {
            peaks.push({ index: i, value: data[i] });
        }
    }

    if (peaks.length === 0) return [];

    // Sort peaks by value in descending order
    peaks.sort((a, b) => b.value - a.value);

    const finalPeaks = [];
    const suppressed = new Array(data.length).fill(false);

    for (const peak of peaks) {
        if (!suppressed[peak.index]) {
            finalPeaks.push(peak.index);
            // Suppress peaks within the distance
            const start = Math.max(0, peak.index - distance);
            const end = Math.min(data.length, peak.index + distance);
            for (let i = start; i < end; i++) {
                suppressed[i] = true;
            }
        }
    }

    // Sort final peaks by index
    finalPeaks.sort((a, b) => a - b);
    return finalPeaks;
}


// --- CHART RENDERING ---

/**
 * Orchestrates the initial processing and rendering of all charts.
 */
function processAndRenderAllCharts() {
    if (sensorData.length === 0) return;
    const timestamps = sensorData.map(d => d.Timestamp_us);
    
    renderRawDataCharts(timestamps);
    renderSmoothedCharts();
    renderSegmentationChart();
    renderDftChart();
}

function renderRawDataCharts(timestamps) {
    if (charts['rawAccChart']) charts['rawAccChart'].destroy();
    if (charts['rawGyroChart']) charts['rawGyroChart'].destroy();
    
    const rawContainer = document.getElementById('raw');
    rawContainer.innerHTML = `<div class="chart-container"><canvas id="rawAccChart"></canvas></div><div class="chart-container"><canvas id="rawGyroChart"></canvas></div>`;
    
    const accData = { AccX: sensorData.map(d => d.AccX), AccY: sensorData.map(d => d.AccY), AccZ: sensorData.map(d => d.AccZ) };
    const gyroData = { GyroX: sensorData.map(d => d.GyroX), GyroY: sensorData.map(d => d.GyroY), GyroZ: sensorData.map(d => d.GyroZ) };
    
    charts['rawAccChart'] = createChart('line', 'rawAccChart', timestamps, accData, 'Dữ Liệu Gia Tốc Kế Gốc');
    charts['rawGyroChart'] = createChart('line', 'rawGyroChart', timestamps, gyroData, 'Dữ Liệu Con Quay Hồi Chuyển Gốc');
}

function renderSmoothedCharts() {
    if (sensorData.length === 0) return;

    if (charts['smoothedAccChart']) charts['smoothedAccChart'].destroy();
    if (charts['smoothedGyroChart']) charts['smoothedGyroChart'].destroy();

    const container = document.querySelector('#smoothed .grid');
    container.innerHTML = `<div class="chart-container"><canvas id="smoothedAccChart"></canvas></div><div class="chart-container"><canvas id="smoothedGyroChart"></canvas></div>`;

    const timestamps = sensorData.map(d => d.Timestamp_us);
    const windowSize = parseInt(maWindowSize.value);

    const accChartData = {
        'Độ lớn Gia tốc Gốc': sensorData.map(d => d.AccMagnitude),
        'Độ lớn Gia tốc Đã làm mịn': movingAverage(sensorData.map(d => d.AccMagnitude), windowSize)
    };
    const gyroChartData = {
        'Độ lớn Con quay Gốc': sensorData.map(d => d.GyroMagnitude),
        'Độ lớn Con quay Đã làm mịn': movingAverage(sensorData.map(d => d.GyroMagnitude), windowSize)
    };

    charts['smoothedAccChart'] = createChart('line', 'smoothedAccChart', timestamps, accChartData, 'So Sánh Độ lớn Gia tốc');
    charts['smoothedGyroChart'] = createChart('line', 'smoothedGyroChart', timestamps, gyroChartData, 'So Sánh Độ lớn Con quay');
}

/**
 * Renders or re-renders the segmentation chart based on UI controls.
 */
function renderSegmentationChart() {
    if (sensorData.length === 0) return;

    if (charts['segmentationChart']) charts['segmentationChart'].destroy();

    const container = document.querySelector('#segmentation .chart-container');
    container.innerHTML = '<canvas id="segmentationChart"></canvas>';

    const timestamps = sensorData.map(d => d.Timestamp_us);
    const selectedSignalName = segmentationSignalSelector.value;
    
    // Get the original signal and smooth it
    const originalSignal = sensorData.map(d => d[selectedSignalName]);
    const windowSize = parseInt(maWindowSize.value); // Use the same smoothing window
    const smoothedSignal = movingAverage(originalSignal, windowSize);

    // Get peak finding parameters from UI
    const threshold = parseFloat(peakThresholdInput.value);
    const distance = parseInt(peakDistanceInput.value);

    // Find peaks on the smoothed signal
    const peakIndices = findPeaks(smoothedSignal, { threshold, distance });
    peakCountSpan.textContent = peakIndices.length;

    const peakData = peakIndices.map(index => ({
        x: timestamps[index],
        y: smoothedSignal[index]
    }));

    const chartData = {
        [selectedSignalName + ' (đã làm mịn)']: smoothedSignal,
    };
    
    const chartTitle = `Phân đoạn theo đỉnh trên ${selectedSignalName}`;
    charts['segmentationChart'] = createChart('line', 'segmentationChart', timestamps, chartData, chartTitle);
    
    // Add peaks as a separate dataset to the existing chart
    if (charts['segmentationChart']) {
        charts['segmentationChart'].data.datasets.push({
            type: 'scatter',
            label: 'Đỉnh (Lần đạp)',
            data: peakData,
            backgroundColor: 'red',
            radius: 5,
        });
        charts['segmentationChart'].update();
    }
}

function renderDftChart() {
    if (charts['dftChart']) charts['dftChart'].destroy();
    
    const dftContainer = document.querySelector('#dft .chart-container');
    dftContainer.innerHTML = `<canvas id="dftChart"></canvas>`;

    const selectedSignalName = dftSignalSelector.value;
    const signal = sensorData.map(d => d[selectedSignalName]);
    if (!signal || signal.length < 2) return;

    const bufferSize = 1 << (Math.floor(Math.log2(signal.length - 1)) + 1);
    const paddedSignal = [...signal];
    while (paddedSignal.length < bufferSize) paddedSignal.push(0);

    const fftResult = math.fft(paddedSignal);
    const magnitudes = fftResult.slice(0, fftResult.length / 2).map(c => Math.sqrt(c.re * c.re + c.im * c.im));
    const frequencies = Array.from({ length: magnitudes.length }, (_, i) => i * (sampleRate / bufferSize));

    const chartTitle = `Phân Tích Tần Số (DFT) của ${selectedSignalName}`;
    charts['dftChart'] = createChart('bar', 'dftChart', frequencies, { 'Biên độ': magnitudes }, chartTitle);
}

// --- UTILITY FUNCTIONS ---

function createChart(type, canvasId, labels, dataSets, title) {
    const colors = ['#3b82f6', '#a8a29e', '#22c55e', '#ef4444', '#8b5cf6', '#f97316'];
    const ctx = document.getElementById(canvasId).getContext('2d');
    
    const chartData = {
        labels: labels,
        datasets: Object.keys(dataSets).map((key, index) => ({
            label: key,
            data: dataSets[key],
            borderColor: colors[index % colors.length],
            backgroundColor: type === 'line' ? colors[index % colors.length] + '1A' : colors[index % colors.length],
            borderWidth: 1.5,
            pointRadius: type === 'line' ? 0 : undefined,
            tension: 0.1
        }))
    };

    const zoomOptions = {
        pan: { enabled: true, mode: type === 'bar' ? 'x' : 'xy' },
        zoom: { wheel: { enabled: true }, pinch: { enabled: true }, mode: type === 'bar' ? 'x' : 'xy' }
    };
    
    const scalesOptions = {
        x: { title: { display: true, text: type === 'bar' ? 'Tần số (Hz)' : 'Timestamp (us)' }, type: type === 'bar' ? 'linear' : undefined, beginAtZero: type === 'bar' },
        y: { title: { display: true, text: type === 'bar' ? 'Biên độ' : 'Giá trị' }, beginAtZero: true }
    };

    return new Chart(ctx, {
        type: type,
        data: chartData,
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: {
                title: { display: true, text: title, font: { size: 16 } },
                legend: { position: 'top', display: Object.keys(dataSets).length > 1 },
                zoom: zoomOptions
            },
            scales: scalesOptions
        }
    });
}

function changeTab(tabName) {
    document.querySelectorAll('.tab-content').forEach(tab => tab.classList.add('hidden'));
    document.getElementById(tabName).classList.remove('hidden');
    document.querySelectorAll('.tab-button').forEach(button => button.classList.remove('active'));
    document.querySelector(`.tab-button[onclick="changeTab('${tabName}')"]`).classList.add('active');
}

function destroyAllCharts() {
    Object.values(charts).forEach(chart => {
        if (chart && typeof chart.destroy === 'function') chart.destroy();
    });
    charts = {};
}
