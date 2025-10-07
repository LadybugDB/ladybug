#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "lbug.h"

int main() {
    lbug_database db;
    lbug_connection conn;
    lbug_database_init("" /* fill db path */, lbug_default_system_config(), &db);
    lbug_connection_init(&db, &conn);

    // Create schema.
    lbug_query_result result;
    lbug_connection_query(
        &conn, "CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name));", &result);
    lbug_query_result_destroy(&result);
    // Create nodes.
    lbug_connection_query(&conn, "CREATE (:Person {name: 'Alice', age: 25});", &result);
    lbug_query_result_destroy(&result);
    lbug_connection_query(&conn, "CREATE (:Person {name: 'Bob', age: 30});", &result);
    lbug_query_result_destroy(&result);

    // Execute a simple query.
    lbug_connection_query(&conn, "MATCH (a:Person) RETURN a.name AS NAME, a.age AS AGE;", &result);

    // Fetch each value.
    lbug_flat_tuple tuple;
    lbug_value value;
    while (lbug_query_result_has_next(&result)) {
        lbug_query_result_get_next(&result, &tuple);

        lbug_flat_tuple_get_value(&tuple, 0, &value);
        char* name;
        lbug_value_get_string(&value, &name);

        lbug_flat_tuple_get_value(&tuple, 1, &value);
        int64_t age;
        lbug_value_get_int64(&value, &age);

        printf("name: %s, age: %" PRIi64 " \n", name, age);
        lbug_destroy_string(name);
    }
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&tuple);

    // Print query result.
    char* result_string = lbug_query_result_to_string(&result);
    printf("%s", result_string);
    lbug_destroy_string(result_string);

    lbug_query_result_destroy(&result);
    lbug_connection_destroy(&conn);
    lbug_database_destroy(&db);
    return 0;
}
