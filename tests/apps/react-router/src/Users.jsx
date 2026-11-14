import { useLoaderData } from 'react-router';

export default function Users() {
  const users = useLoaderData();
  return (
    <section>
      <h1>Users</h1>
      <ul className="users">
        {users.map((user) => (
          <li key={user.id}>
            <strong>{user.name}</strong> <span>{user.city}</span>
          </li>
        ))}
      </ul>
    </section>
  );
}
