CREATE NODE TABLE city(id INT32, name STRING, population INT64, PRIMARY KEY(id)) WITH (storage = 'demo');
CREATE NODE TABLE user(id INT32, name STRING, age INT64, PRIMARY KEY(id)) WITH (storage = 'demo');
CREATE REL TABLE follows(FROM user TO user, since INT32) WITH (storage = 'demo');
CREATE REL TABLE livesin(FROM user TO city) WITH (storage = 'demo');
