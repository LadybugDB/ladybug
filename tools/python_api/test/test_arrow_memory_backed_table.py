import polars as pl
import pytest
from type_aliases import ConnDB


def test_arrow_memory_backed_table_basic(conn_db_empty: ConnDB) -> None:
    """Test basic Arrow memory-backed table creation and querying with polars."""
    conn, _ = conn_db_empty

    # Create a polars DataFrame
    df = pl.DataFrame({
        "id": [1, 2, 3, 4, 5],
        "name": ["Alice", "Bob", "Charlie", "Diana", "Eve"],
        "age": [25, 30, 35, 40, 45],
        "salary": [50000.0, 60000.0, 75000.0, 90000.0, 100000.0],
    })

    # Register the Arrow table
    conn.create_arrow_table("employees", df)

    # Query all data
    result = conn.execute("MATCH (n:employees) RETURN n.id, n.name, n.age, n.salary ORDER BY n.id")
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 5
    assert rows[0] == [1, "Alice", 25, 50000.0]
    assert rows[1] == [2, "Bob", 30, 60000.0]
    assert rows[2] == [3, "Charlie", 35, 75000.0]
    assert rows[3] == [4, "Diana", 40, 90000.0]
    assert rows[4] == [5, "Eve", 45, 100000.0]

    # Clean up
    conn.drop_arrow_table("employees")


def test_arrow_memory_backed_table_filtering(conn_db_empty: ConnDB) -> None:
    """Test filtering rows from an Arrow memory-backed table using Cypher."""
    conn, _ = conn_db_empty

    # Create a polars DataFrame with more data
    df = pl.DataFrame({
        "id": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
        "name": ["Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Henry", "Ivy", "Jack"],
        "age": [25, 30, 35, 40, 45, 28, 33, 38, 42, 50],
        "department": ["Engineering", "Sales", "Engineering", "HR", "Sales",
                       "Engineering", "HR", "Sales", "Engineering", "HR"],
        "salary": [50000.0, 60000.0, 75000.0, 55000.0, 70000.0,
                   52000.0, 58000.0, 65000.0, 80000.0, 60000.0],
    })

    # Register the Arrow table
    conn.create_arrow_table("staff", df)

    # Test 1: Filter by age > 35
    result = conn.execute("MATCH (n:staff) WHERE n.age > 35 RETURN n.name, n.age ORDER BY n.age")
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 5
    assert rows[0] == ["Henry", 38]
    assert rows[1] == ["Diana", 40]
    assert rows[2] == ["Ivy", 42]
    assert rows[3] == ["Eve", 45]
    assert rows[4] == ["Jack", 50]

    # Test 2: Filter by department
    result = conn.execute(
        "MATCH (n:staff) WHERE n.department = 'Engineering' RETURN n.name, n.department ORDER BY n.id"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 4
    assert rows[0] == ["Alice", "Engineering"]
    assert rows[1] == ["Charlie", "Engineering"]
    assert rows[2] == ["Frank", "Engineering"]
    assert rows[3] == ["Ivy", "Engineering"]

    # Test 3: Filter by salary range
    result = conn.execute(
        "MATCH (n:staff) WHERE n.salary >= 60000.0 AND n.salary <= 75000.0 "
        "RETURN n.name, n.salary ORDER BY n.salary"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 5
    assert rows[0] == ["Bob", 60000.0]
    assert rows[1] == ["Jack", 60000.0]
    assert rows[2] == ["Henry", 65000.0]
    assert rows[3] == ["Eve", 70000.0]
    assert rows[4] == ["Charlie", 75000.0]

    # Test 4: Complex filter with AND/OR
    result = conn.execute(
        "MATCH (n:staff) WHERE (n.department = 'Engineering' AND n.salary > 60000.0) "
        "OR n.age > 45 RETURN n.name, n.department, n.salary, n.age ORDER BY n.id"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 3
    assert rows[0] == ["Charlie", "Engineering", 75000.0, 35]
    assert rows[1] == ["Ivy", "Engineering", 80000.0, 42]
    assert rows[2] == ["Jack", "HR", 60000.0, 50]

    # Clean up
    conn.drop_arrow_table("staff")


def test_arrow_memory_backed_table_with_pandas(conn_db_empty: ConnDB) -> None:
    """Test Arrow memory-backed table with pandas DataFrame."""
    conn, _ = conn_db_empty

    pd = pytest.importorskip("pandas")

    # Create a pandas DataFrame
    df = pd.DataFrame({
        "product_id": [101, 102, 103, 104, 105],
        "product_name": ["Widget A", "Widget B", "Gadget X", "Gadget Y", "Tool Z"],
        "price": [9.99, 14.99, 29.99, 34.99, 49.99],
        "in_stock": [True, True, False, True, False],
    })

    # Register the Arrow table
    conn.create_arrow_table("products", df)

    # Query with filter
    result = conn.execute(
        "MATCH (n:products) WHERE n.in_stock = true AND n.price < 20.0 "
        "RETURN n.product_name, n.price ORDER BY n.price"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 2
    assert rows[0] == ["Widget A", 9.99]
    assert rows[1] == ["Widget B", 14.99]

    # Clean up
    conn.drop_arrow_table("products")


def test_arrow_memory_backed_table_with_pyarrow(conn_db_empty: ConnDB) -> None:
    """Test Arrow memory-backed table with native PyArrow table."""
    conn, _ = conn_db_empty

    import pyarrow as pa

    # Create a PyArrow table directly
    table = pa.table({
        "city": ["New York", "Los Angeles", "Chicago", "Houston", "Phoenix"],
        "population": [8419000, 3980000, 2716000, 2328000, 1690000],
        "area_sq_miles": [302.6, 468.7, 227.3, 637.5, 517.6],
    })

    # Register the Arrow table
    conn.create_arrow_table("cities", table)

    # Query with filter
    result = conn.execute(
        "MATCH (n:cities) WHERE n.population > 2000000 AND n.area_sq_miles < 400 "
        "RETURN n.city, n.population, n.area_sq_miles ORDER BY n.population DESC"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 2
    assert rows[0] == ["New York", 8419000, 302.6]
    assert rows[1] == ["Chicago", 2716000, 227.3]

    # Clean up
    conn.drop_arrow_table("cities")


def test_arrow_memory_backed_table_empty_result(conn_db_empty: ConnDB) -> None:
    """Test filtering that returns no results."""
    conn, _ = conn_db_empty

    df = pl.DataFrame({
        "id": [1, 2, 3],
        "value": [10, 20, 30],
    })

    conn.create_arrow_table("data", df)

    # Filter that matches nothing
    result = conn.execute("MATCH (n:data) WHERE n.value > 100 RETURN n.id")
    assert not result.has_next()

    # Clean up
    conn.drop_arrow_table("data")


def test_arrow_memory_backed_table_count(conn_db_empty: ConnDB) -> None:
    """Test aggregation on Arrow memory-backed table."""
    conn, _ = conn_db_empty

    df = pl.DataFrame({
        "category": ["A", "B", "A", "C", "B", "A", "C", "B"],
        "amount": [100, 200, 150, 300, 250, 120, 280, 180],
    })

    conn.create_arrow_table("transactions", df)

    # Count by category
    result = conn.execute(
        "MATCH (n:transactions) RETURN n.category, COUNT(*) as cnt ORDER BY n.category"
    )
    rows = []
    while result.has_next():
        rows.append(result.get_next())

    assert len(rows) == 3
    assert rows[0] == ["A", 3]
    assert rows[1] == ["B", 3]
    assert rows[2] == ["C", 2]

    # Clean up
    conn.drop_arrow_table("transactions")
