import React from 'react';
import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import App from './App.jsx';
import { SocialProvider } from './context/SocialContext.jsx';
import './styles/theme.css';

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <BrowserRouter>
      <SocialProvider>
        <App />
      </SocialProvider>
    </BrowserRouter>
  </React.StrictMode>,
);
