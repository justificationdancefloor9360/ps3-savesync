'use strict';

const API = {
	games: () => fetch('/api/games').then(r => r.json()),
	jobs: () => fetch('/api/jobs').then(r => r.json()),
	exportSave: (id) => fetch(`/api/jobs`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ kind: 'export', save_id: id }),
	}).then(r => r.json()),
	convert: (id, direction) => fetch(`/api/jobs`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ kind: 'convert', save_id: id, direction }),
	}).then(r => r.json()),
	deleteJob: (id) => fetch(`/api/jobs/${id}`, { method: 'DELETE' }),
	inspect: (file) => {
		const fd = new FormData();
		fd.append('file', file);
		return fetch('/api/inspect', { method: 'POST', body: fd })
			.then(r => r.ok ? r.json() : r.json().then(e => Promise.reject(e)));
	},
	importStaged: (payload) => fetch('/api/jobs', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ kind: 'import', ...payload }),
	}).then(r => r.json()),
	status: () => fetch('/api/status').then(r => r.json()),
};

const STATE = {
	games: [],
	jobs: [],
	filter: '',
	expanded: new Set(),
	online: false,
};

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));
const fmt = {
	bytes: (n) => {
		if (!n && n !== 0) return '—';
		const u = ['B', 'KB', 'MB', 'GB'];
		let i = 0; while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
		return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
	},
};

function gameMatches(g, q) {
	if (!q) return true;
	if (g.title.toLowerCase().includes(q)) return true;
	if (g.title_id.toLowerCase().includes(q)) return true;
	return g.slots.some(s => (s.subtitle || '').toLowerCase().includes(q));
}

function renderGames() {
	const grid = $('#save-grid');
	const empty = $('#empty');
	const q = STATE.filter.toLowerCase();
	const visible = STATE.games.filter(g => gameMatches(g, q));

	if (visible.length === 0) {
		grid.innerHTML = '';
		empty.style.display = 'block';
		empty.textContent = STATE.games.length === 0
			? 'No saves found on PS3.'
			: `No matches for "${STATE.filter}".`;
		return;
	}
	empty.style.display = 'none';

	grid.innerHTML = visible.map(g => {
		const open = STATE.expanded.has(g.title_id);
		const slotIcon = g.slots[0]?.dir_name;
		return `
		<div class="game-card${open ? ' open' : ''}" data-tid="${escapeHtml(g.title_id)}">
			<div class="game-head">
				<img class="game-icon" src="/api/saves/${encodeURIComponent(slotIcon)}/icon" alt=""
				     loading="lazy" onerror="this.style.visibility='hidden'">
				<div class="game-meta">
					<p class="game-title">${escapeHtml(g.title || g.title_id)}</p>
					<p class="game-sub">${escapeHtml(g.title_id)} · ${g.slot_count} slot${g.slot_count === 1 ? '' : 's'}</p>
				</div>
				<span class="game-chev">▶</span>
			</div>
			<div class="game-slots">
				${g.slots.map(s => `
					<div class="slot-card" data-id="${escapeHtml(s.id)}">
						<img class="save-icon" src="/api/saves/${encodeURIComponent(s.id)}/icon" alt=""
						     loading="lazy" onerror="this.style.visibility='hidden'"
						     style="width:40px;height:40px">
						<div class="save-meta">
							<p class="save-title">${escapeHtml(s.subtitle || s.dir_name)}</p>
							<p class="save-sub">${escapeHtml(s.dir_name)}</p>
							<div class="save-tags">
								<span class="tag ${s.flavor === 'ps3-signed' ? 'ps3' : 'rpcs3'}">${escapeHtml(s.flavor)}</span>
								${s.location === 'usb' ? '<span class="tag usb">USB</span>' : ''}
								<span class="tag">${fmt.bytes(s.total_size_bytes)}</span>
							</div>
						</div>
					</div>`).join('')}
			</div>
		</div>`;
	}).join('');

	$$('.game-head').forEach(el => el.addEventListener('click', () => {
		const card = el.parentElement;
		const tid = card.dataset.tid;
		if (STATE.expanded.has(tid)) STATE.expanded.delete(tid);
		else STATE.expanded.add(tid);
		card.classList.toggle('open');
	}));
	$$('.slot-card').forEach(el => el.addEventListener('click', e => {
		e.stopPropagation();
		const id = el.dataset.id;
		const slot = STATE.games.flatMap(g => g.slots).find(s => s.id === id);
		if (slot) openSlotDetail(slot);
	}));
}

function openSlotDetail(save) {
	const overlay = $('#detail-overlay');
	const isPs3 = save.flavor === 'ps3-signed';
	$('#detail').innerHTML = `
		<h3>${escapeHtml(save.title || save.dir_name)}</h3>
		<p style="color:var(--dim);font-size:13px;margin:0">${escapeHtml(save.subtitle || '')}</p>
		<dl>
			<dt>Game ID</dt>          <dd>${escapeHtml(save.title_id)}</dd>
			<dt>Save folder</dt>      <dd>${escapeHtml(save.dir_name)}</dd>
			<dt>Format</dt>           <dd>${escapeHtml(save.flavor)}</dd>
			<dt>Location</dt>         <dd>${escapeHtml(save.location)}</dd>
			<dt>Size</dt>             <dd>${fmt.bytes(save.total_size_bytes)} (${save.file_count} files)</dd>
			<dt>Account</dt>          <dd>${save.account_id_hex && save.account_id_hex !== '0000000000000000' ? escapeHtml(save.account_id_hex) : 'unbound (portable)'}</dd>
		</dl>
		<div class="actions">
			<button class="btn" id="close">Close</button>
			${isPs3
				? `<button class="btn primary" id="export">Export for RPCS3</button>`
				: `<button class="btn primary" id="convert">Sign for PS3</button>`}
		</div>`;
	overlay.classList.add('open');
	$('#close').addEventListener('click', () => overlay.classList.remove('open'));
	const expBtn = $('#export');
	if (expBtn) expBtn.addEventListener('click', async () => {
		await API.exportSave(save.id);
		overlay.classList.remove('open');
		refreshJobs();
	});
	const convBtn = $('#convert');
	if (convBtn) convBtn.addEventListener('click', async () => {
		await API.convert(save.id, 'rpcs3-to-ps3');
		overlay.classList.remove('open');
		refreshJobs();
	});
}

/* Build the slot picker after a successful inspect. existingSlots are the
 * dir_names already on the PS3 for this title_id; suggested is what the SFO
 * proposes (e.g. "BLES01807-AUTOSAVE"). The user can replace an existing slot
 * (with optional pre-overwrite backup) or import as a new slot. */
function openImportDialog(inspect) {
	const overlay = $('#detail-overlay');
	const game = STATE.games.find(g => g.title_id === inspect.title_id);
	const existingSlots = game ? game.slots.map(s => s.dir_name) : [];

	const suggested = inspect.suggested_dir_name || `${inspect.title_id}-IMPORT`;
	const suggestedExists = existingSlots.includes(suggested);

	const slotOpts = existingSlots.map(d =>
		`<option value="${escapeHtml(d)}">${escapeHtml(d)}${d === suggested ? ' (suggested)' : ''}</option>`
	).join('');

	$('#detail').innerHTML = `
		<h3>Import: ${escapeHtml(inspect.title || inspect.title_id)}</h3>
		<p style="color:var(--dim);font-size:13px;margin:0 0 4px">${escapeHtml(inspect.subtitle || '')}</p>
		<p style="color:var(--dim);font-size:12px;margin:0 0 12px">${escapeHtml(inspect.title_id)}${suggested ? ` · suggested folder: ${escapeHtml(suggested)}` : ''}</p>
		<form class="import-form" id="import-form">
			<label>
				Target slot
				<select id="slot-mode">
					<option value="new"${suggestedExists ? '' : ' selected'}>New slot</option>
					${existingSlots.length ? `<option value="replace"${suggestedExists ? ' selected' : ''}>Replace existing slot</option>` : ''}
				</select>
			</label>
			<label id="new-slot-row">
				Folder name
				<input type="text" id="new-slot-name" value="${escapeHtml(suggested)}">
			</label>
			<label id="replace-slot-row" style="display:none">
				Existing slot
				<select id="replace-slot">${slotOpts}</select>
			</label>
			<div class="checks">
				<label><input type="checkbox" id="opt-backup" checked> Back up existing slot before replacing</label>
				<label><input type="checkbox" id="opt-overwrite"> Overwrite without backup (cannot be undone)</label>
			</div>
			<p class="hint">A backup renames the existing folder to <code>&lt;name&gt;.bak.&lt;timestamp&gt;</code> on the PS3.</p>
		</form>
		<div class="actions">
			<button class="btn" id="cancel">Cancel</button>
			<button class="btn primary" id="confirm">Import</button>
		</div>`;
	overlay.classList.add('open');

	const slotMode = $('#slot-mode');
	const newRow = $('#new-slot-row');
	const replaceRow = $('#replace-slot-row');
	const updateMode = () => {
		const mode = slotMode.value;
		newRow.style.display = mode === 'new' ? '' : 'none';
		replaceRow.style.display = mode === 'replace' ? '' : 'none';
	};
	slotMode.addEventListener('change', updateMode);
	updateMode();
	if (suggestedExists && existingSlots.length) {
		$('#replace-slot').value = suggested;
	}

	$('#cancel').addEventListener('click', () => overlay.classList.remove('open'));
	$('#confirm').addEventListener('click', async () => {
		const mode = slotMode.value;
		let dir = mode === 'new' ? $('#new-slot-name').value.trim()
		                         : $('#replace-slot').value;
		if (!dir) { alert('Pick a folder name.'); return; }
		const replacing = mode === 'replace' || existingSlots.includes(dir);
		const backup = replacing && $('#opt-backup').checked;
		const overwrite = replacing && ($('#opt-overwrite').checked || !backup);
		await API.importStaged({
			stage_id: inspect.stage_id,
			target_dir_name: dir,
			overwrite: overwrite ? 1 : 0,
			backup: backup ? 1 : 0,
		});
		overlay.classList.remove('open');
		refreshJobs();
	});
}

function renderJobs() {
	const list = $('#jobs');
	const empty = $('#jobs-empty');
	if (STATE.jobs.length === 0) {
		list.innerHTML = ''; empty.style.display = 'block';
		return;
	}
	empty.style.display = 'none';
	list.innerHTML = STATE.jobs.map(j => {
		const pct = j.phase === 'done' ? 100
		          : j.bytes_total > 0 ? Math.round(100 * j.bytes_done / j.bytes_total)
		          : 0;
		const phaseClass = j.phase === 'done' ? 'done' : j.phase === 'failed' ? 'failed' : '';
		return `
		<div class="job">
			<div class="job-row">
				<span class="job-name">${escapeHtml(j.label)}</span>
				<span class="job-phase ${phaseClass}">${escapeHtml(j.phase)}</span>
			</div>
			<div class="bar"><span style="width:${pct}%"></span></div>
			<div class="job-meta">${escapeHtml(j.current_file || '')} ${j.bytes_total ? `· ${fmt.bytes(j.bytes_done)} / ${fmt.bytes(j.bytes_total)}` : ''}</div>
			${j.phase === 'done' && j.download_url
				? `<div style="margin-top:8px"><a class="btn" href="${j.download_url}" download>Download</a></div>`
				: ''}
		</div>`;
	}).join('');
}

function escapeHtml(s) {
	return String(s ?? '').replace(/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	})[c]);
}

async function refreshGames() {
	try {
		STATE.games = await API.games();
		STATE.online = true;
	} catch {
		STATE.online = false;
	}
	updateStatus();
	renderGames();
}

async function refreshJobs() {
	try {
		STATE.jobs = await API.jobs();
	} catch { /* keep last */ }
	renderJobs();
}

async function refreshStatus() {
	try {
		const s = await API.status();
		$('#status-version').textContent = `v${s.version} · ${s.console_id_short || ''}`;
		STATE.online = true;
	} catch {
		STATE.online = false;
	}
	updateStatus();
}

function updateStatus() {
	$('#status-dot').style.background = STATE.online ? 'var(--ok)' : 'var(--err)';
	$('#status-text').textContent = STATE.online ? 'connected' : 'offline';
}

function bindUI() {
	$('#search').addEventListener('input', e => {
		STATE.filter = e.target.value;
		renderGames();
	});
	$('#refresh').addEventListener('click', refreshGames);

	const dz = $('#dropzone');
	const fi = $('#file-input');
	dz.addEventListener('click', () => fi.click());
	fi.addEventListener('change', e => e.target.files[0] && uploadFile(e.target.files[0]));
	['dragenter', 'dragover'].forEach(ev => dz.addEventListener(ev, e => {
		e.preventDefault(); dz.classList.add('dragging');
	}));
	['dragleave', 'drop'].forEach(ev => dz.addEventListener(ev, e => {
		e.preventDefault(); dz.classList.remove('dragging');
	}));
	dz.addEventListener('drop', e => {
		const f = e.dataTransfer.files[0];
		if (f) uploadFile(f);
	});

	$('#detail-overlay').addEventListener('click', e => {
		if (e.target.id === 'detail-overlay') e.currentTarget.classList.remove('open');
	});
}

async function uploadFile(file) {
	if (!file.name.endsWith('.zip')) {
		alert('Please drop a .zip file.');
		return;
	}
	let inspect;
	try {
		inspect = await API.inspect(file);
	} catch (e) {
		alert(`Upload rejected: ${e?.error || 'unknown error'}`);
		return;
	}
	openImportDialog(inspect);
}

async function init() {
	bindUI();
	await Promise.all([refreshStatus(), refreshGames(), refreshJobs()]);
	setInterval(refreshJobs, 1000);
	setInterval(refreshGames, 10000);
	setInterval(refreshStatus, 5000);
}

init();
