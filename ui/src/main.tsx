import React from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './style.css';

const container = document.getElementById('root')!;
// The browser build adds a startup sequence; the native WebView renders directly.
if (__FLOWS_WEB__) void import('./web/boot').then(({ bootWeb }) => bootWeb(container));
else createRoot(container).render(<React.StrictMode><App/></React.StrictMode>);
