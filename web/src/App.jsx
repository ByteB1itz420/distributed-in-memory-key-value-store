import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Activity,
  ArrowDownRight,
  ArrowUpRight,
  Check,
  ChevronRight,
  CircleHelp,
  Clock3,
  Command,
  Database,
  FileKey2,
  KeyRound,
  LoaderCircle,
  Plus,
  RefreshCw,
  RotateCcw,
  Search,
  ShieldCheck,
  Trash2,
  X,
} from "lucide-react";

const apiUrl = (import.meta.env.VITE_API_URL ?? "").replace(/\/+$/, "");

async function responseJson(response) {
  let body;
  try {
    body = await response.json();
  } catch {
    throw new Error("The API returned an invalid response");
  }
  if (!response.ok) throw new Error(body.error ?? `Request failed (${response.status})`);
  return body;
}

async function getJson(path, accessToken, options = {}) {
  const response = await fetch(`${apiUrl}/api${path}`, {
    ...options,
    headers: {
      Authorization: `Bearer ${accessToken}`,
      ...(options.body ? { "Content-Type": "application/json" } : {}),
      ...options.headers,
    },
  });
  return responseJson(response);
}

async function createDemoSession() {
  const response = await fetch(`${apiUrl}/api/demo/session`, { method: "POST" });
  return responseJson(response);
}

function App() {
  const [session, setSession] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const startupPromise = useRef(null);

  useEffect(() => {
    if (!apiUrl) {
      setLoading(false);
      return;
    }
    let active = true;
    if (!startupPromise.current) {
      startupPromise.current = (async () => {
        let token = window.sessionStorage.getItem("kvstore-demo-token");
        if (token) {
          const response = await fetch(`${apiUrl}/api/demo/session`, {
          headers: { Authorization: `Bearer ${token}` },
        });
          if (response.status === 401) {
            window.sessionStorage.removeItem("kvstore-demo-token");
            token = null;
          } else {
            await responseJson(response);
          }
        }
        return token ? { accessToken: token } : createDemoSession();
      })();
    }
    startupPromise.current.then((demo) => {
      if (!active) return;
      window.sessionStorage.setItem("kvstore-demo-token", demo.accessToken);
      setSession({ access_token: demo.accessToken, user: { email: "Demo workspace" } });
      setError("");
    }).catch((cause) => {
      if (active) setError(cause.message);
    }).finally(() => {
      if (active) setLoading(false);
    });
    return () => { active = false; };
  }, []);

  async function startNewDemo() {
    if (!apiUrl) return;
    setSession(null);
    setLoading(true);
    setError("");
    try {
      const body = await createDemoSession();
      window.sessionStorage.setItem("kvstore-demo-token", body.accessToken);
      setSession({ access_token: body.accessToken, user: { email: "Demo workspace" } });
    } catch (cause) {
      setError(cause.message);
    } finally {
      setLoading(false);
    }
  }

  if (!apiUrl) return <ConfigurationScreen />;
  if (loading) return <LoadingScreen />;
  if (!session) return <DemoUnavailable error={error} onRetry={startNewDemo} />;
  return <Console key={session.access_token} session={session} onNewDemo={startNewDemo} />;
}

function ConfigurationScreen() {
  return (
    <main className="config-page">
      <div className="config-card">
        <Brand />
        <h1>Connect the live demo</h1>
        <p>Set the Railway API URL in the Vercel environment variables, then redeploy this project.</p>
        <div className="config-list">
          <code>VITE_API_URL</code>
        </div>
        <p className="config-footnote">This demo connects to the real C++ in-memory store through the Railway API.</p>
      </div>
    </main>
  );
}

function DemoUnavailable({ error, onRetry }) {
  return (
    <main className="config-page">
      <div className="config-card">
        <Brand />
        <h1>Demo connection unavailable</h1>
        <p>{error || "The key-value service could not be reached. It may be starting up; try again in a moment."}</p>
        <button className="button button-primary" onClick={onRetry}><RefreshCw size={15} /> Try again</button>
      </div>
    </main>
  );
}

function LoadingScreen() {
  return (
    <main className="loading-page">
      <LoaderCircle className="spin" size={22} />
      <span>Opening your workspace</span>
    </main>
  );
}

function Console({ session, onNewDemo }) {
  const [keys, setKeys] = useState([]);
  const [status, setStatus] = useState(null);
  const [selectedKey, setSelectedKey] = useState("");
  const [selectedValue, setSelectedValue] = useState("");
  const [ttlInput, setTtlInput] = useState("");
  const [loadingSelectedKey, setLoadingSelectedKey] = useState(false);
  const [selectedKeyError, setSelectedKeyError] = useState("");
  const [selectedLoadAttempt, setSelectedLoadAttempt] = useState(0);
  const [search, setSearch] = useState("");
  const [isNew, setIsNew] = useState(false);
  const [newKey, setNewKey] = useState("");
  const [newValue, setNewValue] = useState("");
  const [newTtl, setNewTtl] = useState("");
  const [busy, setBusy] = useState(false);
  const [loadingKeys, setLoadingKeys] = useState(true);
  const [error, setError] = useState("");
  const [toast, setToast] = useState("");
  const searchRef = useRef(null);

  const accessToken = session.access_token;
  const today = new Intl.DateTimeFormat(undefined, { weekday: "long", month: "long", day: "numeric" }).format(new Date()).toUpperCase();
  const greeting = new Date().getHours() < 12 ? "Good morning" : new Date().getHours() < 18 ? "Good afternoon" : "Good evening";
  const loadKeys = useCallback(async () => {
    setLoadingKeys(true);
    setError("");
    try {
      const [keyData, statusData] = await Promise.all([
        getJson("/keys", accessToken),
        getJson("/status", accessToken),
      ]);
      setKeys(keyData.keys);
      setStatus(statusData);
      if (selectedKey && !keyData.keys.includes(selectedKey)) {
        setSelectedKey("");
        setSelectedValue("");
      }
    } catch (cause) {
      setError(cause.message);
      setStatus({ status: "offline", keyCount: 0 });
    } finally {
      setLoadingKeys(false);
    }
  }, [accessToken, selectedKey]);

  useEffect(() => {
    loadKeys();
    const timer = setInterval(loadKeys, 15_000);
    return () => clearInterval(timer);
  }, [loadKeys]);

  useEffect(() => {
    if (!selectedKey || isNew) {
      setLoadingSelectedKey(false);
      setSelectedKeyError("");
      return;
    }
    let active = true;
    setLoadingSelectedKey(true);
    setSelectedKeyError("");
    getJson(`/keys/${encodeURIComponent(selectedKey)}`, accessToken)
      .then((record) => {
        if (active) {
          setSelectedValue(record.value);
          setTtlInput(record.ttlSeconds > 0 ? String(record.ttlSeconds) : "");
        }
      })
      .catch((cause) => {
        if (active) {
          setSelectedKeyError(cause.message);
          setError(cause.message);
        }
      })
      .finally(() => {
        if (active) setLoadingSelectedKey(false);
      });
    return () => { active = false; };
  }, [accessToken, isNew, selectedKey, selectedLoadAttempt]);

  const visibleKeys = useMemo(() => keys.filter((key) => key.toLowerCase().includes(search.toLowerCase())), [keys, search]);

  useEffect(() => {
    function focusSearch(event) {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
        event.preventDefault();
        searchRef.current?.focus();
      }
    }
    window.addEventListener("keydown", focusSearch);
    return () => window.removeEventListener("keydown", focusSearch);
  }, []);

  async function saveKey(event) {
    event.preventDefault();
    const key = isNew ? newKey.trim() : selectedKey;
    const ttlValue = isNew ? newTtl : ttlInput;
    if (!key) {
      setError("Key name cannot be blank.");
      return;
    }
    setBusy(true);
    setError("");
    try {
      await getJson("/keys", accessToken, {
        method: "POST",
        body: JSON.stringify({
          key,
          value: isNew ? newValue : selectedValue,
          ttlSeconds: ttlValue === "" ? null : Number(ttlValue),
        }),
      });
      setSelectedKey(key);
      setSelectedValue(isNew ? newValue : selectedValue);
      setIsNew(false);
      setToast("Key saved");
      await loadKeys();
    } catch (cause) {
      setError(cause.message);
    } finally {
      setBusy(false);
    }
  }

  async function deleteKey() {
    if (!selectedKey || !window.confirm(`Delete "${selectedKey}"? This cannot be undone.`)) return;
    setBusy(true);
    try {
      await getJson(`/keys/${encodeURIComponent(selectedKey)}`, accessToken, { method: "DELETE" });
      setSelectedKey("");
      setSelectedValue("");
      setToast("Key deleted");
      await loadKeys();
    } catch (cause) {
      setError(cause.message);
    } finally {
      setBusy(false);
    }
  }

  function startNew() {
    setIsNew(true);
    setSelectedKey("");
    setNewKey("");
    setNewValue("");
    setNewTtl("");
    setSelectedValue("");
    setTtlInput("");
    setLoadingSelectedKey(false);
    setSelectedKeyError("");
    setError("");
  }

  function selectKey(key) {
    setIsNew(false);
    setSelectedValue("");
    setTtlInput("");
    setLoadingSelectedKey(true);
    setSelectedKeyError("");
    setSelectedKey(key);
    setSelectedLoadAttempt((attempt) => attempt + 1);
  }

  useEffect(() => {
    if (!toast) return undefined;
    const timer = setTimeout(() => setToast(""), 2600);
    return () => clearTimeout(timer);
  }, [toast]);

  const hasSelection = Boolean(selectedKey) || isNew;

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <Brand />
        <div className="workspace-label">WORKSPACE</div>
        <div className="workspace-chip"><div className="workspace-icon"><Database size={17} /></div><span><strong>My store</strong><small>In-memory demo</small></span><ChevronRight size={15} className="muted-icon" /></div>
        <div className="sidebar-heading"><span>YOUR DATA</span><button className="icon-button plus-button" onClick={startNew} aria-label="Create a key"><Plus size={17} /></button></div>
        <div className="search-box"><Search size={15} /><input ref={searchRef} value={search} onChange={(e) => setSearch(e.target.value)} placeholder="Find a key..." /><kbd>⌘ K</kbd></div>
        <div className="key-list">
          {loadingKeys && keys.length === 0 ? (
            <div className="list-loading"><LoaderCircle className="spin" size={16} /> Loading keys…</div>
          ) : visibleKeys.length ? visibleKeys.map((key) => (
            <button key={key} className={`key-row ${selectedKey === key && !isNew ? "active" : ""}`} onClick={() => selectKey(key)}>
              <FileKey2 size={16} /><span>{key}</span><ChevronRight size={14} className="key-chevron" />
            </button>
          )) : (
            <div className="empty-keys"><div className="empty-icon"><KeyRound size={19} /></div><strong>{search ? "No matches" : "No keys yet"}</strong><span>{search ? "Try a different search." : "Create your first key to get started."}</span>{!search && <button onClick={startNew}><Plus size={14} /> Add a key</button>}</div>
          )}
        </div>
        <div className="sidebar-footer">
          <div className="signed-in"><div className="avatar"><Database size={14} /></div><span><strong>Demo workspace</strong><small>Private demo session</small></span></div>
          <button className="icon-button sign-out" onClick={onNewDemo} title="Start a new demo" aria-label="Start a new demo"><RotateCcw size={16} /></button>
        </div>
      </aside>

      <main className="main-area">
        <header className="topbar">
          <div className="breadcrumb"><span>Workspace</span><ChevronRight size={14} /><strong>Overview</strong></div>
          <div className="topbar-actions">
            <span className={`connection-badge ${status?.status === "online" ? "online" : "offline"}`}><span />{status?.status === "online" ? "Connected" : "Connecting"}</span>
            <button className="icon-button refresh-button" onClick={() => { loadKeys(); if (selectedKey) setSelectedLoadAttempt((attempt) => attempt + 1); }} aria-label="Refresh data"><RefreshCw size={16} /></button>
            <button className="button button-secondary new-demo-button" onClick={onNewDemo}><RotateCcw size={14} /> New demo</button>
            <button className="button button-primary new-button" onClick={startNew}><Plus size={16} /> New key</button>
          </div>
        </header>
        <div className="content">
          <section className="welcome-row">
            <div><div className="eyebrow">{today} <span className="welcome-spark">✳</span></div><h1>{greeting}<span className="heading-period">.</span></h1><p>Here’s what’s happening in your store.</p></div>
            <div className="quick-note"><div className="quick-note-icon"><ShieldCheck size={16} /></div><span><strong>Your space, your keys</strong><small>Your keys are isolated to this demo session.</small></span></div>
          </section>

          <section className="stats-grid">
            <article className="stat-card"><div className="stat-top"><span>KEYS IN STORE</span><div className="stat-icon lilac"><KeyRound size={17} /></div></div><div className="stat-value">{status?.keyCount ?? "—"}</div><div className="stat-bottom"><span className="stat-trend"><ArrowUpRight size={14} /> Live count</span><span>scoped to you</span></div></article>
            <article className="stat-card"><div className="stat-top"><span>SERVER STATUS</span><div className="stat-icon mint"><Activity size={17} /></div></div><div className="stat-value status-value"><i className={`live-dot ${status?.status === "online" ? "on" : ""}`} />{status?.status === "online" ? "Online" : "—"}</div><div className="stat-bottom"><span className="stat-trend">● API reachable</span><span>auto-checks every 15s</span></div></article>
            <article className="stat-card memory-card"><div className="stat-top"><span>DATA LIFETIME</span><div className="stat-icon peach"><Clock3 size={17} /></div></div><div className="stat-value">In memory</div><div className="stat-bottom"><span className="stat-trend muted-trend"><ArrowDownRight size={14} /> Volatile</span><span>cleared on restart</span></div></article>
          </section>

          {error && <div className="inline-alert"><CircleHelp size={17} /><span>{error}</span><button onClick={() => setError("")} aria-label="Dismiss"><X size={16} /></button></div>}
          <section className="data-layout">
            <div className="data-card key-table-card">
              <div className="card-header"><div><div className="card-title">Your keys</div><div className="card-caption">Browse and manage your data</div></div><span className="count-pill">{keys.length} {keys.length === 1 ? "key" : "keys"}</span></div>
              <div className="table-head"><span>KEY</span><span>TYPE</span><span>STATUS</span></div>
              <div className="table-body">
                {visibleKeys.slice(0, 8).map((key) => (
                  <button key={key} className={`table-row ${selectedKey === key && !isNew ? "selected" : ""}`} onClick={() => selectKey(key)}>
                    <span className="table-key"><span className="key-mini-icon"><KeyRound size={14} /></span><strong>{key}</strong></span><span className="type-label">String</span><span className="table-status"><span className="tiny-dot" />Active</span>
                  </button>
                ))}
                {!visibleKeys.length && <div className="table-empty">{search ? "No keys match your search." : "Nothing here yet. Add a key to get started."}</div>}
                {visibleKeys.length > 8 && <div className="more-keys">Showing 8 of {visibleKeys.length} keys · use search to find more</div>}
              </div>
              <div className="table-footer"><span><span className="tiny-dot" /> Demo data scoped to this session</span><button onClick={loadKeys}>Refresh <RefreshCw size={13} /></button></div>
            </div>

            <section className="data-card editor-card">
              <div className="card-header editor-header"><div><div className="card-title">{isNew ? "Create a key" : selectedKey ? "Key details" : "Quick start"}</div><div className="card-caption">{isNew ? "Add a new value to your store" : selectedKey ? "Edit this key’s value" : "Select a key or create something new"}</div></div>{hasSelection && <span className="editing-badge"><span /> EDITING</span>}</div>
              {hasSelection ? (
                <form className="editor-form" onSubmit={saveKey}>
                  <label>KEY NAME<input value={isNew ? newKey : selectedKey} onChange={(e) => isNew && setNewKey(e.target.value)} readOnly={!isNew} placeholder="e.g. user:42:profile" autoFocus={isNew} required /></label>
                  <label>VALUE<textarea value={isNew ? newValue : selectedValue} onChange={(e) => isNew ? setNewValue(e.target.value) : setSelectedValue(e.target.value)} placeholder="Enter a string value…" rows={7} disabled={!isNew && (loadingSelectedKey || Boolean(selectedKeyError))} /></label>
                  <label>EXPIRES IN <span className="optional-label">OPTIONAL</span><div className="ttl-input"><input type="number" min="1" max="31536000" value={isNew ? newTtl : ttlInput} onChange={(e) => isNew ? setNewTtl(e.target.value) : setTtlInput(e.target.value)} placeholder={isNew ? "Until session expires" : "Keep current expiry"} disabled={!isNew && (loadingSelectedKey || Boolean(selectedKeyError))} /><span>seconds</span></div></label>
                  <div className="editor-actions"><button type="submit" className="button button-primary save-button" disabled={busy || loadingSelectedKey || Boolean(selectedKeyError)}>{busy ? <LoaderCircle className="spin" size={15} /> : <Check size={15} />}{loadingSelectedKey ? "Loading key…" : isNew ? "Create key" : "Save changes"}</button>{!isNew && <button type="button" className="button button-danger" onClick={deleteKey} disabled={busy || loadingSelectedKey || Boolean(selectedKeyError)}><Trash2 size={15} /> Delete</button>}<button type="button" className="button button-quiet" onClick={() => { setIsNew(false); setSelectedKey(""); setSelectedValue(""); setTtlInput(""); }}>Cancel</button></div>
                </form>
              ) : (
                <div className="quick-start">
                  <div className="quick-start-visual"><div className="qs-orbit orbit-one" /><div className="qs-orbit orbit-two" /><div className="qs-center"><Command size={23} /></div><div className="qs-chip chip-a"><KeyRound size={15} /></div><div className="qs-chip chip-b"><Database size={14} /></div></div>
                  <h3>Make yourself at home.</h3><p>Create a key to start storing values. Your browser demo session keeps your keys neatly organized.</p>
                  <button className="button button-secondary" onClick={startNew}><Plus size={15} /> Create your first key</button>
                  <div className="quick-start-tip"><CircleHelp size={14} /> Values are text and may optionally expire.</div>
                </div>
              )}
            </section>
          </section>
          <footer className="page-footer"><span><span className="footer-mark">KV</span> A little space for your data.</span><span>Separate demo session <ShieldCheck size={13} /></span></footer>
        </div>
      </main>
      {toast && <div className="toast"><Check size={16} />{toast}</div>}
    </div>
  );
}

function Brand({ light = false }) {
  return <div className={`brand ${light ? "brand-light" : ""}`}><div className="brand-mark"><Command size={17} strokeWidth={2.5} /></div><span>kv<span className="brand-dot">.</span>store</span></div>;
}

export default App;
