import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { createBrowserRouter, RouterProvider } from 'react-router';
import { BASENAME, USERS_URL } from './paths.js';
import Layout from './Layout.jsx';
import Home from './Home.jsx';
import Users from './Users.jsx';
import './app.css';

async function usersLoader() {
  const response = await fetch(USERS_URL);
  if (!response.ok) throw new Response('Could not load users', { status: response.status });
  return response.json();
}

const router = createBrowserRouter(
  [
    {
      path: '/',
      Component: Layout,
      children: [
        { index: true, Component: Home },
        { path: 'users', Component: Users, loader: usersLoader },
        {
          path: 'about',
          lazy: async () => {
            const module = await import('./About.jsx');
            return { Component: module.default };
          },
        },
      ],
    },
  ],
  { basename: BASENAME },
);

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <RouterProvider router={router} />
  </StrictMode>,
);
