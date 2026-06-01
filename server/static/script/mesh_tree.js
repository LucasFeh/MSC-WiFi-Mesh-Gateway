const LAYER_PALETTE = ['#16a34a', '#2563eb', '#d97706', '#ec4899', '#06b6d4', '#475569'];
const collapsedMacs = new Set();
let treeRoot = null;

function layerColor(layer) {
    if (layer == null) return '#475569';
    return LAYER_PALETTE[(layer - 1) % LAYER_PALETTE.length];
}

function buildHierarchy(devices) {
    const byMac = {};
    devices.forEach(d => { byMac[d.mac] = { ...d, children: [] }; });
    const roots = [];
    devices.forEach(d => {
        if (d.parent && byMac[d.parent])
            byMac[d.parent].children.push(byMac[d.mac]);
        else
            roots.push(byMac[d.mac]);
    });
    return { name: 'WiFi Router', virtual: true, children: roots };
}

// ── D3 setup ──────────────────────────────────────────────────────────────
const svg  = d3.select('#treesvg');
const g    = svg.append('g').attr('class', 'zoom-group');
const tree = d3.tree().nodeSize([160, 80]);
const zoom = d3.zoom()
    .scaleExtent([0.1, 3])
    .on('zoom', e => g.attr('transform', e.transform));
svg.call(zoom);

function centerTree() {
    const w = svg.node().clientWidth || 800;
    svg.call(zoom.transform, d3.zoomIdentity.translate(w / 2, 60));
}

// ── collapse state ─────────────────────────────────────────────────────────
function applyCollapseState(root) {
    root.each(n => {
        if (n.data.mac && collapsedMacs.has(n.data.mac) && n.children) {
            n._children = n.children;
            n.children  = null;
        }
    });
}

// ── render ─────────────────────────────────────────────────────────────────
function renderTree(devices) {
    const isFirst = treeRoot === null;
    treeRoot      = d3.hierarchy(buildHierarchy(devices), d => d.children);
    treeRoot.x0   = 0;
    treeRoot.y0   = 0;
    applyCollapseState(treeRoot);
    update(treeRoot);
    if (isFirst) centerTree();
    document.getElementById('updated').textContent = new Date().toLocaleTimeString();
}

function update(source) {
    tree(treeRoot);
    const nodes = treeRoot.descendants();
    const links = treeRoot.descendants().slice(1);

    // ── nodes ──────────────────────────────────────────────────────────────
    const node   = g.selectAll('g.node').data(nodes, d => d.data.mac || 'root');
    const sx0    = source.x0 ?? 0;
    const sy0    = source.y0 ?? 0;

    const nEnter = node.enter().append('g')
        .attr('class', 'node')
        .attr('transform', `translate(${sx0},${sy0})`)
        .style('cursor', 'pointer')
        .on('click', (_, d) => {
            if (!d.data.mac) return;
            if (d.children) {
                collapsedMacs.add(d.data.mac);
                d._children = d.children;
                d.children  = null;
            } else {
                collapsedMacs.delete(d.data.mac);
                d.children  = d._children;
                d._children = null;
            }
            update(d);
        });

    nEnter.append('rect')
        .attr('x', -70).attr('y', -15).attr('width', 140).attr('height', 30).attr('rx', 6)
        .attr('fill', '#1e293b')
        .attr('stroke', d => d.data.virtual ? '#475569' : layerColor(d.data.layer))
        .attr('stroke-width', 1.5);

    nEnter.filter(d => !d.data.virtual).append('circle')
        .attr('cx', -55).attr('cy', 0).attr('r', 3.5)
        .attr('fill', d => d.data.online ? '#22c55e' : '#ef4444');

    nEnter.append('text')
        .attr('dy', '0.35em')
        .attr('x', d => d.data.virtual ? 0 : -44)
        .style('text-anchor', d => d.data.virtual ? 'middle' : 'start')
        .style('font-family', '"Cascadia Mono", Consolas, monospace')
        .style('font-size', '11px')
        .style('fill', d => d.data.virtual ? '#94a3b8' : (d.data.online ? '#e2e8f0' : '#64748b'))
        .text(d => d.data.virtual ? 'WiFi Router' : d.data.mac.slice(-8).toUpperCase());

    nEnter.filter(d => !d.data.virtual).append('text')
        .attr('dy', '0.35em').attr('x', 55)
        .style('text-anchor', 'end').style('font-size', '10px').style('font-weight', '700')
        .style('fill', d => layerColor(d.data.layer))
        .text(d => d.data.layer ? `L${d.data.layer}` : '');

    const nMerge = nEnter.merge(node);
    const t = nMerge.transition().duration(400)
        .attr('transform', d => `translate(${d.x},${d.y})`);
    t.select('rect')
        .style('opacity', d => (!d.data.virtual && !d.data.online) ? 0.45 : 1);
    t.select('circle')
        .attr('fill', d => d.data.online ? '#22c55e' : '#ef4444');
    t.select('text')
        .style('fill', d => d.data.virtual ? '#94a3b8' : (d.data.online ? '#e2e8f0' : '#64748b'));

    node.exit().transition().duration(400)
        .attr('transform', `translate(${source.x ?? 0},${source.y ?? 0})`).remove();

    // ── links ──────────────────────────────────────────────────────────────
    const diag = d3.linkVertical().x(d => d.x).y(d => d.y);
    const link = g.selectAll('path.link').data(links, d => d.data.mac || 'root');

    link.enter().insert('path', 'g')
        .attr('class', 'link')
        .attr('fill', 'none').attr('stroke', '#334155').attr('stroke-width', 1.5)
        .attr('d', () => { const o = {x: sx0, y: sy0}; return diag({source: o, target: o}); })
        .merge(link).transition().duration(400)
        .attr('d', d => diag({source: d.parent, target: d}));

    link.exit().transition().duration(400)
        .attr('d', () => {
            const o = {x: source.x ?? 0, y: source.y ?? 0};
            return diag({source: o, target: o});
        }).remove();

    nodes.forEach(d => { d.x0 = d.x; d.y0 = d.y; });
}

// ── data ───────────────────────────────────────────────────────────────────
const socket = io();
socket.on('state_update', s => renderTree(s.devices));
fetch('/api/state')
    .then(r => r.json())
    .then(s => renderTree(s.devices))
    .catch(console.error);
