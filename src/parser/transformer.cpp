
        } else if (content[i] == '\'' && i + 1 < content.length() && content[i + 1] == '\'') {
            // SQL-standard single quote escape: '' → '
            result += '\'';
            i++;
        } else {
            result += content[i];
        }
    ]

    return result;
}