import React from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './style.css';

const container = document.getElementById('root')!;
// The browser build adds a startup sequence; the native WebView renders directly.
if (__FLOWS_WEB__) {
  import('./web/boot').then(({ bootWeb }) => bootWeb(container)).catch(error => {
    // The startup code itself failed to load, usually a dropped connection.
    console.error(error);
    createRoot(container).render(<div className="boot-screen" role="alert"><div className="boot-panel">
      <h1>Flows could not load</h1>
      <p>Part of the page did not download. Check your connection, then reload.</p>
      <button className="primary-button" onClick={() => location.reload()}>Reload</button>
    </div></div>);
  });
}
else createRoot(container).render(<React.StrictMode><App/></React.StrictMode>);
