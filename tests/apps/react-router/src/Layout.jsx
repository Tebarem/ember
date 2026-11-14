import { NavLink, Outlet } from 'react-router';

export default function Layout() {
  return (
    <div className="shell">
      <nav className="nav">
        <NavLink to="/" end>Home</NavLink>
        <NavLink to="/users">Users</NavLink>
        <NavLink to="/about">About</NavLink>
      </nav>
      <main className="page">
        <Outlet />
      </main>
    </div>
  );
}
